# Technical Decision Record

This document records the current technical decisions for the TestBridge starter
kit. It describes implementation context, not a frozen public API.

## 1. Keep A Small Reference Host

Decision: keep `testbridge_lab` as a minimal Qt Quick host instead of binding
the automation layer to a product application.

Rationale: a reusable automation framework needs a repeatable target with low
business-domain coupling.

Tradeoff: the starter still contains small compatibility shims such as the
logger, config access, and `GuiDataBus`. These shims are reference-host support
code, not required infrastructure for every consuming application.

## 2. Make The Qt Provider Explicit

Decision: the validated Windows workflow uses a user-provided Qt 5.15 prefix via
`QT_PREFIX` or `CMAKE_PREFIX_PATH`.

Rationale: different environments may use system Qt, Conan, vcpkg, or another
package source. Open-source documentation should not contain machine-specific Qt
paths.

Tradeoff: developers must configure their own Qt prefix before running the
Windows bootstrap script.

## 3. Manage bgfx Through A Submodule

Decision: keep `external/bgfx.cmake` as the top-level bgfx integration
submodule.

Rationale: bgfx, bx, bimg, shaderc, and meshoptimizer should be pinned to a
reviewable version.

Tradeoff: new clones must initialize nested submodules with:

```powershell
git submodule update --init --recursive
```

## 4. Start With The Terrain Heightfield Renderer

Decision: `engine_core` currently builds the terrain renderer, Qt Quick render
viewport item, required shaders, and support code.

Rationale: this is the smallest 3D surface needed to validate Qt Quick + bgfx +
TestBridge automation.

Tradeoff: `src/engine/common/nanovg` and `src/engine/common/ps` remain source
inventory and are not part of the active engine API.

## 5. Embed bgfx Through QQuickFramebufferObject

Decision: expose `RenderViewportItem` as a `QQuickFramebufferObject`-based QML
item.

Rationale: this keeps the migrated offscreen rendering path working while still
letting agents find the render surface through QML `objectName` hooks.

Tradeoff: hosts must configure Qt for the OpenGL scenegraph path before
constructing `QGuiApplication`. The current offscreen readback/upload path is
portable for testing but not the highest-performance production integration.

## 6. Bind TestBridge To Loopback

Decision: `WsServer` listens on `127.0.0.1` by default and currently allows one
active client.

Rationale: the bridge is intended for local development and CI. Loopback is the
security boundary when no authentication layer is enabled.

Tradeoff: multi-agent, remote, or distributed testing requires a separate
transport and authorization design.

## 7. Keep The Event Bus RPC Surface

Decision: keep `bus.publish`, `bus.subscribe`, `bus.unsubscribe`, and `bus.wait`
as the host event synchronization API.

Rationale: event synchronization is useful for automation and avoids polling
when the app can publish state changes.

Tradeoff: applications that do not use `GuiDataBus` should adapt the bus RPCs to
their own event source.

## 8. Keep The MCP Layer Independent

Decision: C++ TestBridge exposes WebSocket JSON-RPC; the Agent-facing tool layer
lives in `tools/testbridge-mcp`.

Rationale: this keeps the C++ runtime small and lets Agent tooling evolve
quickly.

Tradeoff: both the C++ protocol and Python MCP wrapper must stay compatible.
Mock MCP tests and real GUI smoke tests should both remain in CI.

## 9. Treat End-To-End Smoke Tests As The Main Gate

Decision: the primary validation path launches the real GUI application and
drives it through TestBridge.

Rationale: Qt initialization, QML traversal, QTest input, log capture,
screenshots, shader lookup, and application shutdown are integration behaviors.

Tradeoff: CI needs a display-capable environment or a documented headless
strategy.
