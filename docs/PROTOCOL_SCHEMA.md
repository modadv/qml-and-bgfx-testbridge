# Protocol Schema

The protocol is JSON-RPC 2.0 over WebSocket. `app.describe` is the capability
entry point. Agents should check for a method before using it.

## Stable Method Groups

- `app.*`: lifecycle and protocol discovery.
- `qml.*`: QObject/QQuickItem introspection and input.
- `window.*`: screenshot capture.
- `log.*`: recent logs and log wait.
- `render.*`: host renderer snapshots.
- `shader.*`: live shader compile/apply/revert/list.
- `test.*`: artifact collection metadata.

## Error Contract

RPC errors use JSON-RPC error objects. Handlers should return structured
diagnostics in `data` where possible. Agent workflows should treat unavailable
providers as a capability failure, not a transport failure.

Example:

```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "error": {
    "code": -32602,
    "message": "params.handle required",
    "data": {"method": "qml.meta"}
  }
}
```

## Capability Manifest

`app.describe` should include:

- application identity
- Qt/platform information
- protocol version
- supported method list
- visible windows

Template-level metadata lives in `template/manifest.json`.

## Stable Response Fields

Agents may rely on these field groups:

| RPC | Stable Fields |
|---|---|
| `qml.geometry` | `scene.x`, `scene.y`, `scene.w`, `scene.h`, `visible`, `enabled`, `isQuickItem` |
| `qml.meta` | `class`, `objectName`, `properties`, `methods`, `signals`, `quickItem` |
| `qml.tree` | `items`, `count`, `truncated`, `limit` |
| `render.caps` | `available`, `backend`, `tier`, `noCompute`, `renderer`, `initialized`, `lastFrameId` |
| `render.stats` | `available`, engine timing fields, `bgfx.stats` when bgfx is initialized |
| `render.resources` | `available`, render item name, asset paths, resource tables, `liveShader` |
| `shader.list` | `available`, `supportedSlots`, `records` |

Numeric bgfx handle IDs are diagnostics only. Agents should assert validity
flags and named resource state instead of depending on exact handle values.
