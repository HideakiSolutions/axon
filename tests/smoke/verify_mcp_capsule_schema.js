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
for (const field of ['path', 'source_ref', 'expand_command', 'content', 'tokens']) {
  if (!(field in pivot)) throw new Error(`missing pivot field: ${field}`);
}
if (!pivot.source_ref.includes('src/auth/token.ts')) {
  throw new Error(`unexpected source_ref: ${pivot.source_ref}`);
}
if (!pivot.expand_command.includes('get_skeleton') &&
    !pivot.expand_command.includes('get_context_capsule')) {
  throw new Error(`unexpected expand_command: ${pivot.expand_command}`);
}

if (typeof capsule.token_estimate !== 'number' || capsule.token_estimate <= 0) {
  throw new Error('missing token estimate');
}
if (capsule.token_estimate > 1000) {
  throw new Error(`capsule exceeded requested budget: ${capsule.token_estimate}`);
}
if (!capsule.compression || typeof capsule.compression.tokens_saved !== 'number') {
  throw new Error('missing compression counters');
}
if (!Array.isArray(capsule.ccr_artifact_ids)) {
  throw new Error('missing CCR artifact id array');
}
if (capsule.cache !== 'miss') {
  throw new Error(`expected no_cache miss path, got ${capsule.cache}`);
}

console.log('mcp_capsule_schema_ok=true');
