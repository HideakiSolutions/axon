#!/usr/bin/env python3
"""Dependency-free DOM/accessibility and fail-closed HTTP smoke for Axon Web."""
from html.parser import HTMLParser
from pathlib import Path
import http.client
import base64
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import time
import re


class Dom(HTMLParser):
    def __init__(self):
        super().__init__()
        self.elements = []

    def handle_starttag(self, tag, attrs):
        self.elements.append((tag, dict(attrs)))


def response(port, path, headers=None, method="GET"):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
    connection.request(method, path, headers=headers or {})
    result = connection.getresponse()
    body = result.read().decode()
    connection.close()
    return result.status, result.getheader("Content-Type", ""), int(result.getheader("Content-Length", "0")), body


def b64url(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode()


def tlv(data, offset=0):
    tag = data[offset]
    size = data[offset + 1]
    offset += 2
    if size & 0x80:
        width = size & 0x7f
        size = int.from_bytes(data[offset:offset + width], "big")
        offset += width
    return tag, data[offset:offset + size], offset + size


def jwks_from_key(key):
    der = subprocess.check_output(["openssl", "rsa", "-in", str(key), "-pubout", "-outform", "DER"], stderr=subprocess.DEVNULL)
    _, spki, _ = tlv(der)
    _, _, offset = tlv(spki)  # algorithm identifier
    _, bit_string, _ = tlv(spki, offset)
    _, rsa, _ = tlv(bit_string[1:])  # leading unused-bit count
    _, modulus, offset = tlv(rsa)
    _, exponent, _ = tlv(rsa, offset)
    return json.dumps({"keys": [{"kty": "RSA", "kid": "portfolio-web-smoke", "use": "sig", "n": b64url(modulus.lstrip(b"\0")), "e": b64url(exponent.lstrip(b"\0"))}]})


def token(key, issuer, audience):
    header = b64url(json.dumps({"alg": "RS256", "kid": "portfolio-web-smoke"}, separators=(",", ":")).encode())
    claims = b64url(json.dumps({"iss": issuer, "aud": audience, "sub": "portfolio-web-smoke", "exp": int(time.time()) + 60}, separators=(",", ":")).encode())
    signing = (header + "." + claims).encode()
    signature = subprocess.check_output(["openssl", "dgst", "-sha256", "-sign", str(key)], input=signing)
    return header + "." + claims + "." + b64url(signature)


def run_browser_journey(html):
    """Execute the shipped page script against a tiny DOM; no browser framework or network."""
    script = re.search(r"<script>(.*?)</script>", html, re.DOTALL).group(1)
    harness = r'''
const source = require('fs').readFileSync(0, 'utf8');
class Element { constructor(id=''){this.id=id;this.value='';this.textContent='';this.children=[];this.listeners={};}
  addEventListener(name, listener){this.listeners[name]=listener;} append(value){this.children.push(value);} appendChild(value){this.children.push(value);}
  replaceChildren(){this.children=[];this.textContent='';} focus(){this.focused=true;} }
const ids=['t','go','state','topology-note','top','edges','detail','graph-root','fragment','drift'];
const elements=Object.fromEntries(ids.map(id=>[id,new Element(id)]));
global.document={getElementById:id=>elements[id],createElement:()=>new Element(),createTextNode:value=>String(value)};
global.fetch=async path=>{let body={}; if(path==='/api/v1/portfolio/status')body={degraded:false};
 else if(path.startsWith('/api/v1/portfolio/topology'))body={truncated:false,nodes:[{id:'repo:stream:a',name:'alpha',repository_id:'repo',path:'src/a.ts',epoch:'e1',contracts:['A'],routes:['GET /a']},{id:'other:stream:b',name:'beta',repository_id:'other',path:'src/b.ts',epoch:'e1',contracts:['B'],routes:[]}],edges:[{id:'candidate',source:'repo:stream:a',target:'other:stream:b',classification:'convergent_capability',score:.8}]};
 else if(path.startsWith('/api/v1/capabilities/consumers/'))body={repositories:['consumer-repo'],consumer_capabilities:['consumer-repo:stream:c'],unresolved_import_specifiers:['shared-lib'],evidence:['src/a.ts']};
 else if(path.startsWith('/api/v1/capabilities/compare/'))body={classification:'convergent_capability',score:.8,differences:['routes differ'],invalidators:[]};
 else if(path.startsWith('/api/v1/capabilities/drift?'))body={matches:2,drift:1}; else throw Error('unexpected path '+path); return {ok:true,status:200,json:async()=>body};};
(async()=>{eval(source); await elements.go.listeners.click(); if(elements.top.children.length!==2||elements.edges.children.length!==1)throw Error('topology did not render');
 await elements.top.children[0].listeners.click(); if(!elements.detail.children.join('').includes('consumer-repo:stream:c')||!elements.detail.children.join('').includes('shared-lib'))throw Error('consumer/package detail did not render');
 await elements.edges.children[0].listeners.click(); if(!elements.detail.children.join('').includes('Comparison convergent_capability'))throw Error('comparison did not render');
 elements['graph-root'].value='/registered';elements.fragment.value='fragment.json';await elements.drift.listeners.click();if(!elements.detail.children.join('').includes('Declared matches: 2'))throw Error('drift did not render');if(elements.state.textContent!=='Fresh')throw Error('fresh status did not render');})().catch(error=>{console.error(error);process.exitCode=1;});
'''
    subprocess.run(["node", "-e", harness], input=script, text=True, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)


def connected_screenshot(port, bearer, destination, profile):
    """Capture an authenticated, interacted page without storing the bearer in browser state."""
    debug_port = random.randrange(35000, 44000)
    browser = subprocess.Popen([
        "google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu",
        "--remote-allow-origins=*",
        f"--remote-debugging-port={debug_port}", f"--user-data-dir={profile}",
        "--window-size=1280,900", f"http://127.0.0.1:{port}/portfolio",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(40):
            try:
                connection = http.client.HTTPConnection("127.0.0.1", debug_port, timeout=.25)
                connection.request("GET", "/json")
                pages = json.loads(connection.getresponse().read())
                connection.close()
                target = next(page for page in pages if page.get("type") == "page")
                break
            except (OSError, StopIteration, json.JSONDecodeError):
                time.sleep(.1)
        else:
            raise RuntimeError("Chrome DevTools endpoint was unavailable")
        # Node's built-in WebSocket is sufficient here; the short-lived token is supplied only to
        # the live DOM field and never placed in a query string, file, localStorage or cookie.
        control = r'''
const fs=require('fs'); const [url,bearer,output]=process.argv.slice(1); const ws=new WebSocket(url); let next=1; const pending=new Map();
ws.onmessage=e=>{const m=JSON.parse(e.data);const p=pending.get(m.id);if(p){pending.delete(m.id);m.error?p.reject(Error(m.error.message)):p.resolve(m.result)}};
function call(method,params={}){return new Promise((resolve,reject)=>{const id=next++;pending.set(id,{resolve,reject});ws.send(JSON.stringify({id,method,params}))})}
const wait=ms=>new Promise(resolve=>setTimeout(resolve,ms));
ws.onopen=async()=>{try{await call('Runtime.enable');await call('Page.enable');const encoded=JSON.stringify(bearer);await call('Runtime.evaluate',{expression:`document.getElementById('t').value=${encoded};document.getElementById('go').click()`});for(let i=0;i<40;i++){await wait(100);const r=await call('Runtime.evaluate',{expression:"document.getElementById('state').textContent+'|'+document.querySelectorAll('#top button').length",returnByValue:true});if(r.result.value.startsWith('Fresh|')&&!r.result.value.endsWith('|0'))break;if(i===39)throw Error('authenticated topology did not render')}await call('Runtime.evaluate',{expression:"document.querySelector('#top button').click()"});await wait(150);const png=await call('Page.captureScreenshot',{format:'png'});fs.writeFileSync(output,Buffer.from(png.data,'base64'));ws.close()}catch(error){console.error(error);process.exitCode=1;ws.close()}};
'''
        result = subprocess.run(["node", "-e", control, target["webSocketDebuggerUrl"], bearer, str(destination)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError("Chrome interaction failed: " + result.stderr.decode(errors="replace"))
        assert destination.is_file() and destination.stat().st_size > 1024, "connected screenshot was not created"
    finally:
        browser.terminate()
        browser.wait(timeout=5)


def main(binary):
    temporary = Path(tempfile.mkdtemp(prefix="axon-portfolio-web-"))
    process = None
    try:
        project = temporary / "project"
        registry = temporary / "registry"
        base_environment = os.environ | {"AXON_REGISTRY_DIR": str(registry)}
        (project / ".git").mkdir(parents=True)
        (project / ".git" / "HEAD").write_text("ref: refs/heads/main\n")
        (project / "src").mkdir()
        (project / "src" / "fixture.ts").write_text("export const portfolioFixture = true;\n")
        subprocess.run([binary, "index", "."], cwd=project, env=base_environment, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # The browser is read-only. Populate the derived catalog before starting the process that
        # owns the project index, so the smoke does not turn a display request into a writer.
        sync = subprocess.run([binary, "portfolio", "sync"], cwd=project, env=base_environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert sync.returncode in (0, 2), f"portfolio sync returned unexpected code: {sync.returncode}"
        # Reuse the real-DuckDB catalog fixture so browser delivery is exercised against actual
        # capabilities, dependencies and read-only source provenance rather than only a mock.
        fixture = Path(tempfile.gettempdir()) / "axon-g13-catalog"
        fixture_test = Path(binary).parent / "tests" / "test_portfolio_capability_catalog"
        subprocess.run([str(fixture_test)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        shutil.copy2(fixture / "catalog.duckdb", fixture / "registry" / "portfolio-capability-catalog.duckdb")
        # Drift reads an immutable Git blob from a registered source root.  Build a real local
        # fixture rather than treating the delivery handler as a mock-only concern.
        declaration = fixture / "repo" / "capabilities.json"
        declaration.write_text(json.dumps({"schema_version": "axon/capability-graph/v1", "capabilities": [
            {"id": "payments.observed", "name": "payment", "contracts": []},
            {"id": "identity.orphan", "name": "identity provision", "contracts": []},
        ]}))
        for command in (("init", "--quiet"), ("config", "user.email", "axon-smoke@example.invalid"),
                        ("config", "user.name", "Axon smoke"), ("add", "capabilities.json"),
                        ("commit", "--quiet", "-m", "declaration fixture")):
            subprocess.run(["git", *command], cwd=fixture / "repo", check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        key = temporary / "key.pem"
        subprocess.run(["openssl", "genrsa", "-out", str(key), "2048"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        issuer, audience = "https://keycloak.example.test/realms/axon", "axon-portfolio-api"
        environment = base_environment | {"AXON_REGISTRY_DIR": str(fixture / "registry"), "AXON_KEYCLOAK_ISSUER": issuer, "AXON_KEYCLOAK_AUDIENCE": audience, "AXON_KEYCLOAK_JWKS_JSON": jwks_from_key(key)}
        authorization = {"Authorization": "Bearer " + token(key, issuer, audience)}
        port = random.randrange(24000, 34000)
        process = subprocess.Popen([binary, "serve", "--http", f"--port={port}"], cwd=project, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(40):
            if process.poll() is not None:
                raise RuntimeError(process.stdout.read())
            try:
                status, content_type, _, html = response(port, "/portfolio")
                if status == 200:
                    break
            except OSError:
                time.sleep(.25)
        else:
            raise RuntimeError("portfolio web server did not start")
        dom = Dom()
        dom.feed(html)
        assert content_type.startswith("text/html"), f"portfolio route must serve HTML: {content_type}"
        elements = {attrs.get("id"): (tag, attrs) for tag, attrs in dom.elements if attrs.get("id")}
        required_ids = {"t", "go", "state", "top", "edges", "detail", "graph-root", "fragment", "drift"}
        missing = required_ids - elements.keys()
        assert not missing, f"missing DOM ids: {sorted(missing)}"
        assert elements["t"][1].get("type") == "password"
        assert elements["t"][1].get("autocomplete") == "off"
        assert elements["state"][1].get("aria-live") == "polite"
        assert elements["detail"][1].get("tabindex") == "-1"
        assert "https://" not in html and "http://" not in html and "cdn." not in html.lower()
        for marker in ("/api/v1/portfolio/topology?limit=", "/api/v1/capabilities/consumers/", "/api/v1/capabilities/compare/", "/api/v1/capabilities/drift?graph_root=", "encodeURIComponent", "prefers-reduced-motion", "Unresolved import specifiers"):
            assert marker in html, f"missing UI behavior marker: {marker}"
        run_browser_journey(html)
        status, _, _, _ = response(port, "/api/v1/portfolio/status")
        assert status == 401, f"portfolio API must fail closed without a bearer token: {status}"
        status, content_type, response_size, payload = response(port, "/api/v1/portfolio/topology?limit=100", authorization)
        assert status == 200 and content_type.startswith("application/json"), f"authenticated topology failed: {status} {content_type}"
        assert response_size <= 1_000_000, f"topology response exceeded 1 MiB: {response_size}"
        topology = json.loads(payload)
        assert 0 < len(topology["nodes"]) <= 200
        assert all(edge["source"] in {node["id"] for node in topology["nodes"]} and edge["target"] in {node["id"] for node in topology["nodes"]} for edge in topology["edges"])
        if topology["edges"]:
            status, _, _, payload = response(port, "/api/v1/capabilities/compare/" + topology["edges"][0]["id"], authorization)
            assert status == 200 and json.loads(payload)["id"] == topology["edges"][0]["id"], "topology edge IDs must remain usable compare identifiers"
        status, _, _, _ = response(port, "/api/v1/portfolio/topology?limit=101", authorization)
        assert status == 400, "topology must reject a request above its server-side bound"
        payment = next(node for node in topology["nodes"] if node["path"] == "src/payment.cpp")
        status, _, _, payload = response(port, "/api/v1/capabilities/consumers/" + payment["id"], authorization)
        assert status == 200, f"authenticated consumers failed: {status}"
        consumers = json.loads(payload)
        assert "@axon/shared-contracts" in consumers["unresolved_import_specifiers"]
        assert len(consumers["consumer_capabilities"]) <= 100
        status, _, _, _ = response(port, "/api/v1/capabilities/consumers/" + payment["id"] + "?limit=101", authorization)
        assert status == 400, "consumer details must reject a request above its server-side bound"
        status, _, _, payload = response(port, "/api/v1/capabilities/duplicates?threshold=0&limit=10", authorization)
        assert status == 200, f"authenticated duplicates failed: {status}"
        candidates = json.loads(payload)["candidates"]
        assert candidates, "multi-repository fixture did not produce a candidate"
        status, _, _, payload = response(port, "/api/v1/capabilities/compare/" + candidates[0]["id"], authorization)
        assert status == 200 and json.loads(payload)["id"] == candidates[0]["id"], "authenticated compare must resolve a real candidate"
        drift_path = "/api/v1/capabilities/drift?graph_root=" + str(fixture / "repo") + "&fragment=capabilities.json"
        status, _, _, payload = response(port, drift_path, authorization)
        assert status == 200, f"authenticated drift failed: {status} {payload}"
        drift = json.loads(payload)
        assert drift["matches"] >= 1 and drift["drift"] >= 1, "drift fixture must cover declared and observed gaps"
        screenshot = os.environ.get("AXON_PORTFOLIO_WEB_SCREENSHOT")
        if screenshot:
            connected_screenshot(port, authorization["Authorization"].removeprefix("Bearer "), Path(screenshot), temporary / "chrome-profile")
    finally:
        if process is not None:
            process.terminate()
            process.wait(timeout=5)
        shutil.rmtree(temporary)


if __name__ == "__main__":
    main(os.path.realpath(sys.argv[1]))
