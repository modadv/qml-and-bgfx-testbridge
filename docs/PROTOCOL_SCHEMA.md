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

## Capability Manifest

`app.describe` should include:

- application identity
- Qt/platform information
- protocol version
- supported method list
- visible windows

Template-level metadata lives in `template/manifest.json`.
