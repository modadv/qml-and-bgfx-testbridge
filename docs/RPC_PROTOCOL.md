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
