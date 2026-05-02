# TestBridge Lab Architecture

Updated: 2026-05-01

This document describes the current implementation boundaries, runtime flow,
verification paths, and known risks for this repository. It records current
facts and does not treat migration-stage internals as a stable public API.

## Purpose

`testbridge-lab` is a clean reference workspace for evolving TestBridge into a
general, cross-platform automation framework for Qt GUI and bgfx graphics
applications.

The validated loop is:

1. Build a minimal Qt Quick application.
2. Embed a bgfx-backed 3D render surface in QML.
3. Start an in-process WebSocket JSON-RPC TestBridge endpoint.
4. Let scripts or MCP tools inspect, drive, screenshot, and wait on logs.
5. Use real UI smoke tests to validate development and debugging workflows.

## Top-Level Layout

```text
.
|-- CMakeLists.txt
|-- conanfile.py
|-- external/bgfx.cmake
|-- src/app
|-- src/testbridge
|-- src/engine
|-- src/core/logger
|-- src/quick
|-- src/config
|-- tools/testbridge-mcp
|-- tests/smoke_testbridge_lab.py
```

Responsibilities:

| Path | Responsibility |
|---|---|
| `src/app` | Minimal Qt Quick host that loads QML, registers the 3D item, and starts TestBridge. |
| `src/testbridge` | In-process WebSocket JSON-RPC automation bridge. |
| `src/engine` | bgfx terrain heightfield renderer and Qt Quick adapter. |
| `src/core/logger` | spdlog-compatible logging shim for the reference host. |
| `src/quick` | `GuiDataBus` event-bus shim for sample synchronization. |
| `src/config` | Minimal config shim needed by the engine sample. |
| `tools/testbridge-mcp` | Python MCP wrapper for Agent tools. |
| `tests` | End-to-end tests that launch the real application. |

## Build Graph

The root CMake project requires CMake 3.26, C++14, and Qt 5.15 components:

```text
Core, Gui, Qml, Quick, QuickControls2, Network, OpenGL, Test, WebSockets
```

Main target relationship:

```text
testbridge_lab
        +-- testbridge
        |       +-- Qt5::WebSockets
        |       +-- Qt5::Test
        |       +-- spdlog
        |       +-- nlohmann_json
        |
        +-- engine_core
                +-- engine_shaders
                +-- Qt Quick/OpenGL
                +-- bgfx, bx, bimg
                +-- meshoptimizer
```

`engine_shaders` uses `shaderc` from `external/bgfx.cmake`. On Windows the
current shader build emits `dx11`, `glsl`, and `spirv` variants. The app
post-build step copies QML files and compiled shaders next to the executable
because the bgfx loader resolves `shaders/<renderer>/*.bin` relative to the
working directory.

## Dependencies

Conan-managed packages:

```text
spdlog/1.14.1
nlohmann_json/3.11.2
qt/5.15.11, when use_system_qt=False
```

Open-source workflows should provide Qt through `QT_PREFIX` or
`CMAKE_PREFIX_PATH` instead of hard-coded machine paths:

```powershell
$env:QT_PREFIX = "<path-to-qt>"
conan install . -of .build-release -s build_type=Release -o '&:use_system_qt=True' -c user.build:system_qt_prefix="$env:QT_PREFIX" --build=missing
cmake -S . -B .build-release\build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=.build-release\build\generators\conan_toolchain.cmake -DCMAKE_PREFIX_PATH="$env:QT_PREFIX"
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
```

## bgfx Submodules

`external/bgfx.cmake` is the top-level bgfx integration submodule. The public
`.gitmodules` entry should use HTTPS:

```ini
[submodule "external/bgfx.cmake"]
    path = external/bgfx.cmake
    url = https://github.com/modadv/bgfx.cmake.git
```

`external/bgfx.cmake` also depends on nested submodules:

```text
external/bgfx.cmake/bgfx
external/bgfx.cmake/bimg
external/bgfx.cmake/bx
```

After a fresh clone, run:

```powershell
git submodule update --init --recursive
```

## Runtime Flow

`src/app/main.cpp` is the reference host entry point:

1. Set `QT_OPENGL=desktop`.
2. Set `QSG_RENDER_LOOP=basic`.
3. Set `Qt::AA_UseDesktopOpenGL`.
4. Create `QGuiApplication`.
5. Register `RenderViewportItem` in `TestBridgeLab.Engine 1.0`.
6. Generate sample heightfield/diffuse PNGs under `assets/`.
7. Inject `labController`, `sampleHeightfieldUrl`, and `sampleDiffuseUrl`.
8. Load `qml/Main.qml`.
9. Start `testbridge::TestBridge` on `TESTBRIDGE_PORT`, default
   `47600`.

Stable QML object names in the sample UI:

```text
main_window
lab_increment_click
lab_reset_click
lab_counter_label
lab_status_label
lab_engine_view_3d
```

`LabController` provides the sample counter behavior. Clicking the increment
button updates QML text, writes a log line, and publishes `lab.counterChanged`
through `GuiDataBus`.

## TestBridge Runtime

`src/testbridge` is a static library in the neutral namespace:

```cpp
testbridge
```

Key classes:

| Class | Responsibility |
|---|---|
| `TestBridge` | Singleton facade that creates the endpoint and registers RPC methods. |
| `WsServer` | Loopback `QWebSocketServer`; currently single-client. |
| `RpcDispatcher` | JSON-RPC 2.0 parsing, dispatch, response, and error wrapping. |
| `QmlProbe` | QObject/QQuickItem discovery, property access, method calls, and input injection. |
| `ScreenGrabber` | `QWindow` screenshot capture as base64 PNG. |
| `LogSink` | Bounded spdlog sink with regex search and wait support. |
| `BusEndpoint` | Adapter from `GuiDataBus` to JSON-RPC. |

Current RPC surface:

```text
app.ping
app.version
app.describe
app.quit

bus.publish
bus.subscribe
bus.unsubscribe
bus.wait

qml.list
qml.find
qml.get
qml.set
qml.invoke
qml.click
qml.type
qml.geometry
qml.meta
qml.mouse
qml.key
qml.tree
qml.hit

window.grab

log.recent
log.wait

render.caps
render.stats
render.resources
test.artifacts

shader.compile
shader.apply
shader.revert
shader.list
```

Important implementation details:

| Capability | Current Implementation |
|---|---|
| QML discovery | Traverses both `QObject::children()` and `QQuickItem::childItems()`. |
| Handles | Converts QObject pointers to `quint64` and guards them with `QPointer`. |
| Input | Uses `QTest` against the target object's `QQuickWindow`. |
| Tree dumps | Walks each `QQuickWindow::contentItem()`. |
| Hit testing | Recursively finds visible `QQuickItem` instances at scene coordinates. |
| Screenshots | Returns base64 PNG over WebSocket. |
| Logs | Uses a bounded spdlog sink with `log.recent` and `log.wait`. |
| Bus events | Emits JSON-RPC notifications named `bus.event`. |

## MCP Layer

`tools/testbridge-mcp` is an independent Python package using FastMCP. Defaults:

```text
TESTBRIDGE_HOST=127.0.0.1
TESTBRIDGE_PORT=47600
TESTBRIDGE_MAX_SIZE=16777216
```

MCP tools wrap the C++ JSON-RPC methods so an Agent can use the bridge without
hand-writing protocol messages. MCP unit tests use a mock WebSocket server;
real GUI behavior is covered by `tests/smoke_testbridge_lab.py`.

## 3D Engine Module

`src/engine` currently builds the active terrain heightfield render path:

```text
common/bgfx_utils.cpp
terrain/render_device.cpp
terrain/terrain_cpu_compute.cpp
terrain/performance_monitor.cpp
terrain/terrain_renderer.cpp
terrain/terrain_patch_tables.cpp
terrain/render_capabilities.cpp
terrain/terrain_uniforms.cpp
quick/render_viewport_item.cpp
quick/render_scene.cpp
quick/asset_fetch_service.cpp
```

Qt Quick integration:

1. `RenderViewportItem` derives from `QQuickFramebufferObject`.
2. QML properties synchronize into `RenderScene`.
3. `RenderScene` owns camera, overlay, input, and renderer state.
4. `RenderDevice` owns the global bgfx lifecycle, surfaces, views, render
   targets, and readback resources.
5. bgfx renders into an offscreen texture; the current path reads back and
   uploads into the Qt FBO texture.

The readback/upload path is useful for a portable test framework, but it has
latency and bandwidth costs. Production engines may replace it with shared GL
textures, direct FBO integration, QRhi, or a scenegraph-native path.

## Verified Workflow

```powershell
ctest --test-dir .build-release\build -C Release --output-on-failure
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tools\testbridge-mcp\testbridge_mcp\tests -q
```

The smoke test covers:

1. Launching the real `testbridge_lab.exe`.
2. Waiting for the TestBridge port.
3. Validating `app.ping`, `app.version`, and `app.describe`.
4. Finding QML object names.
5. Clicking `lab_increment_click`.
6. Reading `lab_counter_label.text`.
7. Waiting for a counter log line.
8. Capturing a PNG screenshot.
9. Inspecting render caps, stats, and resources.
10. Exercising the live shader path when enabled.
11. Calling `app.quit`.

## Current Risks

| Risk | Notes |
|---|---|
| Qt provider setup | Users must provide a Qt 5.15 prefix with WebSockets support. |
| Debug runtime stability | Mixed Debug/Release Qt runtimes may be unstable on the QFBO path. |
| Recursive submodules | `bgfx.cmake` needs bgfx/bimg/bx initialized recursively. |
| Single-client WebSocket | The current transport is loopback and single-client. |
| No authentication | Loopback binding is the security boundary. Do not expose it to a network. |
| Idle timeout | Long-running clients should call `app.ping` periodically. |
| Engine target scope | `engine_core` currently combines Qt adapter, bgfx renderer, asset loading, and service code. |
| Readback performance | bgfx offscreen-to-Qt readback/upload is expensive. |

## Next Architecture Steps

1. Keep public package names neutral and avoid application-specific wording.
2. Split TestBridge into public API, transport, protocol, Qt probing, logging,
   screenshot, and artifact layers.
3. Replace `GuiDataBus` with a generic host event interface or document it as a
   reference-host adapter.
4. Define a stable host integration contract for startup, shutdown, ports,
   logging, screenshots, and object roots.
5. Version the JSON-RPC protocol and schema.
6. Keep CMake install/export targets working through package consumer tests.
7. Keep Qt provider selection explicit through presets or documented cache vars.
8. Expand CI across Windows Release and at least one display-backed Linux path.
9. Move inactive engine source inventory into staging or separate targets.
10. Add more layered smoke tests for QML-only, render-surface, logging,
    screenshots, and large QML tree scenarios.
