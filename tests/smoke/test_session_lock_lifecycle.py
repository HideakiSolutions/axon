#!/usr/bin/env python3
"""Regression coverage for idle lease release and frozen lock owners."""

from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait_for_owner(registry: Path, pid: int, present: bool, timeout: float = 8.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            repos = json.loads(registry.read_text(encoding="utf-8")).get("repos", [])
        except (FileNotFoundError, json.JSONDecodeError):
            repos = []
        found = any(int(repo.get("owner_pid", 0)) == pid for repo in repos)
        if found == present:
            return
        time.sleep(0.1)
    raise AssertionError(f"owner pid={pid} present={present} not observed")


def main() -> int:
    axon = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="axon_session_lock_") as scratch:
        root = Path(scratch)
        project = root / "project"
        registry_dir = root / "registry"
        project.mkdir()
        registry_dir.mkdir()
        (project / "main.py").write_text("def answer():\n    return 42\n", encoding="utf-8")

        environment = os.environ.copy()
        environment["AXON_REGISTRY_DIR"] = str(registry_dir)
        environment["AXON_DB_IDLE_SECONDS"] = "1"
        environment["AXON_PEER_TIMEOUT_MS"] = "200"
        subprocess.run([str(axon), "init", str(project)], env=environment, check=True,
                       stdout=subprocess.DEVNULL)
        subprocess.run([str(axon), "index", str(project)], env=environment, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)

        owner = subprocess.Popen(
            [str(axon), "serve"], cwd=project, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
        )
        registry = registry_dir / "registry.json"
        try:
            wait_for_owner(registry, owner.pid, True)

            # The client connection remains open, simulating a detached parent
            # session. Axon must release only the DB lease and stay alive.
            wait_for_owner(registry, owner.pid, False)
            assert owner.poll() is None, "idle serve exited instead of releasing only its DB lease"
            status = subprocess.run(
                [str(axon), "status"], cwd=project, env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
            )
            assert status.returncode == 0, status.stderr.decode(errors="replace")

            request = {
                "jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "get_overview", "arguments": {"limit": 1}},
            }
            assert owner.stdin is not None
            owner.stdin.write((json.dumps(request) + "\n").encode())
            owner.stdin.flush()
            wait_for_owner(registry, owner.pid, True)

            # A live but frozen owner must be reported and must not hold a
            # latecomer for the historical five-minute socket timeout.
            os.kill(owner.pid, signal.SIGSTOP)
            started = time.monotonic()
            doctor = subprocess.run(
                [str(axon), "doctor", "locks", "--json"], cwd=project, env=environment,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4,
            )
            assert doctor.returncode == 2
            report = json.loads(doctor.stdout)
            assert report["owners"][0]["status"] == "unresponsive"
            registry_token = json.loads(registry.read_text(encoding="utf-8"))["repos"][0][
                "owner_token"
            ]
            assert registry_token.encode() not in doctor.stdout
            assert b"owner_token" not in doctor.stdout
            assert time.monotonic() - started < 3.0

            started = time.monotonic()
            latecomer = subprocess.run(
                [str(axon), "serve"], cwd=project, env=environment,
                input=(json.dumps(request) + "\n").encode(),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4,
            )
            elapsed = time.monotonic() - started
            assert latecomer.returncode == 0
            assert elapsed < 3.0, f"latecomer remained blocked for {elapsed:.2f}s"
            assert b"within 200ms" in latecomer.stdout, latecomer.stdout.decode(errors="replace")
        finally:
            try:
                os.kill(owner.pid, signal.SIGCONT)
            except ProcessLookupError:
                pass
            if owner.stdin:
                owner.stdin.close()
            try:
                owner.wait(timeout=4)
            except subprocess.TimeoutExpired:
                owner.terminate()
                owner.wait(timeout=4)

    print("session_lock_lifecycle_ok=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
