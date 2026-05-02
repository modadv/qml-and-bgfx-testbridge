# Agent Integration

This project exposes a pluggable Qt QML + bgfx automation layer for AI Coding
Agents. The host app links `TestBridge::testbridge`, starts the bridge after QML
loads, and optionally registers render providers.

## CMake Integration

Install from this repo:

```powershell
cmake --install .build-release\build --config Release --prefix <install-prefix>
```

Consume from another project:

```cmake
find_package(TestBridge CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE TestBridge::testbridge)
```

Host startup:

```cpp
auto& bridge = testbridge::TestBridge::instance();
bridge.setRenderCapsProvider([] { return nlohmann::json{{"available", false}}; });
bridge.setRenderStatsProvider([] { return nlohmann::json{{"available", false}}; });
bridge.setRenderResourcesProvider([] { return nlohmann::json{{"available", false}}; });
bridge.start(&app, port);
```

Stop it from `QCoreApplication::aboutToQuit`.

## Agent RPC Surface

- `app.describe`: app identity, Qt/platform data, visible windows, protocol
  version, and supported RPC methods.
- `qml.list/find/get/set/invoke`: object discovery and property/method access.
- `qml.geometry/meta/tree/hit`: object geometry, visual state, QObject
  properties, methods, signals, full rendered QQuickItem tree, and hit testing.
- `window.grab`: base64 PNG screenshot.
- `log.recent/wait`: recent logs and blocking log assertions.
- `render.caps`: bgfx capabilities and backend tier.
- `render.stats`: host-provided frame/performance counters.
- `render.resources`: host-provided scene/resource state.
- `test.artifacts`: artifact collection capability metadata.

The Python MCP server exposes matching tools so an Agent can use the same
capabilities without manually writing JSON-RPC.

## Render Provider Contract

`src/testbridge` does not depend on `src/engine`. Render data is injected by the
host through JSON providers. A bgfx host should expose ordinary values only:
backend tier, feature flags, frame IDs, performance timings, resource validity,
scene size, loaded asset paths, and any renderer-specific readiness flags. Do not
return bgfx handles or require TestBridge to call bgfx directly.

## Failure Artifacts

The real GUI smoke test writes artifacts on failure. Set
`TESTBRIDGE_ARTIFACT_DIR` to control the output directory. Each failure contains:

- `manifest.json`: `app.describe`, `qml.tree`, `qml.meta`, `render.*`,
  `log.recent`, and screenshot metadata.
- `screenshot.png`: current app window capture.
- `app.log`: copied app process log.

Use `TESTBRIDGE_FORCE_ARTIFACT_FAILURE=1` to verify the failure collection path.
