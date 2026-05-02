# RPC Protocol

TestBridge exposes a loopback WebSocket JSON-RPC 2.0 endpoint for local
development and CI automation.

## Required Startup

- Set `TESTBRIDGE_PORT` or use the default port configured by the app.
- Launch the desktop app with `ENABLE_TESTBRIDGE=ON`.
- Connect MCP or a raw WebSocket client to `127.0.0.1:<port>`.

## Core RPC Groups

- `app.*`: version, app description, protocol metadata, and shutdown.
- `qml.*`: object discovery, property reads/writes, method invocation, input
  injection, geometry, meta-object data, tree dumps, and hit tests.
- `window.*`: screenshot capture of the full Qt window.
- `render.*`: bgfx caps, performance counters, resource state, scene state, and
  render surface metadata.
- `shader.*`: controlled live shader compile, apply, revert, and cache listing.
- `test.*`: artifact directory and failure capture capabilities.

See `schemas/testbridge-protocol.schema.json` for the machine-readable
contract and `docs/PROTOCOL_SCHEMA.md` for Agent usage rules.

## Minimal JSON-RPC Examples

Request:

```json
{"jsonrpc":"2.0","id":1,"method":"app.describe","params":{}}
```

Successful response:

```json
{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"1.0","methods":["app.describe","qml.tree","render.stats"]}}
```

Error response:

```json
{"jsonrpc":"2.0","id":1,"error":{"code":-32601,"message":"Method not found"}}
```

## Agent Capability Checks

Agents should call `app.describe` first and verify a method exists before using
it. Treat an unavailable render provider as a capability failure, not a
transport failure.

Recommended first-pass sequence:

1. `app.describe`
2. `qml.tree`
3. `render.caps`
4. `render.stats`
5. `render.resources`
6. `window.grab`

## Live Shader Sequence

```json
{"jsonrpc":"2.0","id":10,"method":"shader.list","params":{}}
```

```json
{"jsonrpc":"2.0","id":11,"method":"shader.compile","params":{"slot":"terrain_simple.fragment","source":"$input v_texcoord0\nvoid main() { gl_FragColor = vec4(1.0); }\n"}}
```

```json
{"jsonrpc":"2.0","id":12,"method":"shader.apply","params":{"slot":"terrain_simple.fragment","hash":"<compile-result-hash>"}}
```

After apply, poll `render.resources.liveShader.slots` until the matching slot
reports `activeHash == <compile-result-hash>`. Then use `window.grab` or a
golden oracle to verify the visual result.

```json
{"jsonrpc":"2.0","id":13,"method":"shader.revert","params":{"slot":"terrain_simple.fragment"}}
```
