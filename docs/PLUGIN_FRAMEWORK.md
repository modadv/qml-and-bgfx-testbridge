# Pluggable TestBridge Framework Direction

This project is moving from a lab app toward a pluggable automation framework
for AI coding agents that need to test Qt Quick/QML UIs and bgfx render
surfaces.

## Framework Contract

A host application should only need to provide these integration points:

| Area | Contract |
|---|---|
| Startup | Start TestBridge after QML roots and render items are created. Startup must report success or fail fast. |
| Transport | Bind the JSON-RPC WebSocket to loopback by default. Non-loopback exposure requires a separate security decision. |
| Object model | Give stable `objectName` values to agent-facing QML controls and render surfaces. |
| Rendering | Expose render surfaces as QQuickItems so geometry, screenshot, and pixel checks can target them. |
| Logging | Attach the TestBridge log sink once per process and keep bounded in-memory history. |
| Shutdown | Let automation call `app.quit`; release transport resources before process exit. |

The current lab app is the reference host. It should remain small, but the
TestBridge library should not depend on this specific app.

## Agent-Facing Capabilities

The minimum portable capability set is:

| Capability | RPC surface |
|---|---|
| Health/version | `app.ping`, `app.version` |
| QML discovery | `qml.find`, `qml.tree`, `qml.hit`, `qml.geometry` |
| QML state | `qml.get`, `qml.set`, `qml.invoke` |
| Input | `qml.click`, `qml.mouse`, `qml.key`, `qml.type` |
| Screenshot | `window.grab` |
| Logs | `log.recent`, `log.wait` |
| Host events | `bus.publish`, `bus.subscribe`, `bus.wait` |

Long waits must not block the GUI event loop. If an RPC can wait for future UI
work, implement it as an asynchronous operation or move it off the GUI thread.

## Test Tiers

| Tier | Purpose |
|---|---|
| MCP unit tests | Validate Python tool behavior against a mock bridge. |
| Real UI smoke | Launch the reference app and drive QML through TestBridge. |
| Render smoke | Validate the bgfx surface is not blank or uniform, and assert the expected render tier logs. |
| Render matrix | Run at least `TESTBRIDGE_RENDER_TIER=full` and `TESTBRIDGE_RENDER_TIER=nocompute`. |
| Failure-path tests | Verify port conflicts and missing app paths fail clearly. |

CTest should be the common entry point for CI, with pytest still usable directly
while developing.

## Current Gaps Before Calling It Pluggable

1. Export/install a CMake package for the TestBridge library.
2. Keep public package names and app-specific wording neutral as the framework
   grows.
3. Make startup success observable in the public API.
4. Move blocking wait RPCs off the GUI-thread dispatch path.
5. Replace fixed local Qt/build paths with presets or documented cache vars.
6. Add CI jobs for Windows Release and at least one Linux display-backed path.
7. Add render assertions that detect blank frames, stale frames, and missing
   shader resources.
