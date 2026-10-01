const fs = require('fs');

const raw = fs.readFileSync(process.argv[2], 'utf8');
const match = raw.match(/\r?\n\r?\n([\s\S]*)$/);
const body = (match ? match[1] : raw).trim();
const rpc = JSON.parse(body);

if (rpc.error) throw new Error(`JSON-RPC error: ${JSON.stringify(rpc.error)}`);
if (!rpc.result || rpc.result.isError) {
  throw new Error(`tool returned error: ${JSON.stringify(rpc.result)}`);
}

const text = rpc.result.content?.[0]?.text;
if (typeof text !== 'string') throw new Error('missing text tool result');
const capsule = JSON.parse(text);

if (!Array.isArray(capsule.pivot_files) || capsule.pivot_files.length === 0) {
  throw new Error('missing pivot files');
}

const pivot = capsule.pivot_files[0];
// Compact wire format: no per-file expand command or token count, one `expand` hint instead.
for (const field of ['path', 'source_ref', 'content']) {
  if (!(field in pivot)) throw new Error(`missing pivot field: ${field}`);
}
for (const field of ['expand_command', 'tokens']) {
  if (field in pivot) throw new Error(`pivot field should be omitted from the compact format: ${field}`);
}
if (!pivot.source_ref.includes('src/auth/token.ts')) {
  throw new Error(`unexpected source_ref: ${pivot.source_ref}`);
}
if (typeof capsule.expand !== 'string' || !capsule.expand.includes('get_symbol')) {
  throw new Error(`missing expand hint: ${capsule.expand}`);
}

if (typeof capsule.token_estimate !== 'number' || capsule.token_estimate <= 0) {
  throw new Error('missing token estimate');
}
if (capsule.token_estimate > 1000) {
  throw new Error(`capsule exceeded requested budget: ${capsule.token_estimate}`);
}
// Compression counters and CCR ids only appear when compression actually ran.
if ('compression' in capsule || 'ccr_artifact_ids' in capsule) {
  throw new Error('empty compression/CCR sections must be omitted');
}
if (capsule.cache !== 'miss') {
  throw new Error(`expected no_cache miss path, got ${capsule.cache}`);
}

console.log('mcp_capsule_schema_ok=true');
