# TestBridge Lab Validation Checklist

Use this checklist when validating changes to the starter kit. The goal is to
preserve the Agent automation loop, not just to prove that the project compiles.

## Repository State

```powershell
git status --short --ignored
git submodule status --recursive
git config --get submodule.external/bgfx.cmake.url
```

Expected state:

| Area | Expectation |
|---|---|
| Worktree | No build outputs, logs, virtualenvs, or pytest cache are staged. |
| Top-level submodule | `external/bgfx.cmake` exists. |
| Nested submodules | `bgfx`, `bimg`, and `bx` are initialized. |
| `.gitmodules` | Uses a public canonical remote. |
| Local Git config | May contain local overrides, but documentation must not rely on them. |

After a fresh clone, run:

```powershell
git submodule update --init --recursive
```

## Build Verification

Generic Windows Release flow:

```powershell
$env:QT_PREFIX = "<path-to-qt>"
conan install . -of .build-release -s build_type=Release -o '&:use_system_qt=True' -c user.build:system_qt_prefix="$env:QT_PREFIX" --build=missing
cmake -S . -B .build-release\build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=.build-release\build\generators\conan_toolchain.cmake -DCMAKE_PREFIX_PATH="$env:QT_PREFIX"
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
```

Confirm:

| Area | Expectation |
|---|---|
| Qt | `Qt5::WebSockets` is available. |
| bgfx | `shaderc`, `bgfx`, `bx`, `bimg`, and `meshoptimizer` targets exist. |
| Shaders | Compiled shaders exist under `.build-release/build/bin/shaders`. |
| Post-build copy | `qml/` and `shaders/` are copied next to the executable. |
| App target | `testbridge_lab.exe` is generated. |

## Runtime Verification

Manual run:

```powershell
$env:QT_PREFIX = "<path-to-qt>"
$env:PATH = "$env:QT_PREFIX\bin;" + $env:PATH
$env:TESTBRIDGE_PORT = "47670"
.\.build-release\build\bin\Release\testbridge_lab.exe
```

Connect the MCP server to the same port:

```powershell
tools\testbridge-mcp\.venv\Scripts\testbridge-mcp.exe --host 127.0.0.1 --port 47670
```

## Smoke Test

```powershell
tools\testbridge-mcp\.venv\Scripts\python.exe tests\smoke_testbridge_lab.py
```

The smoke test must cover:

| Capability | Check |
|---|---|
| Startup | The app launches and the TestBridge port opens. |
| App RPC | `app.ping` returns `pong`; `app.version` returns a version. |
| QML discovery | The main window, controls, labels, and 3D surface are discoverable. |
| QML tree | `qml.list`, `qml.tree`, and `qml.hit` expose named and visible controls. |
| Input | `qml.click`, `qml.mouse`, and `qml.key` can trigger user-equivalent events. |
| State readback | `qml.get` can read the counter text. |
| Log wait | `log.wait` can observe the counter log line. |
| Screenshot | `window.grab` returns a PNG. |
| Render caps | `render.caps` reports backend, tier, compute flags, and device state. |
| Render stats | `render.stats` reports performance and bgfx frame counters. |
| Render resources | `render.resources` reports assets, textures, programs, buffers, and live shader state. |
| Lifecycle | `app.quit` exits the application. |

## Live Shader Verification

```powershell
ctest --test-dir .build-release\build -C Release -L shader --output-on-failure
```

The full-tier test must compile, apply, observe, and revert:

| Slot | Stage |
|---|---|
| `terrain_simple.vertex` | Vertex |
| `terrain_simple.fragment` | Fragment |
| `overlay_max_elevation.compute` | Compute |

The NoCompute test must cover vertex and fragment slots and skip compute when
`render.caps.noCompute` is true.

## MCP Unit Tests

```powershell
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tools\testbridge-mcp\testbridge_mcp\tests -q
```

These tests use a mock WebSocket server and do not need a running Qt app. They
validate the Python MCP layer and JSON-RPC client behavior.

## Validation Focus

| Area | Check |
|---|---|
| JSON-RPC | Preserve JSON-RPC 2.0 response and error shapes. |
| QML traversal | Keep both `QObject::children()` and `QQuickItem::childItems()` traversal. |
| Handles | Keep `QPointer` protection for pointer-derived handles. |
| GUI thread | Keep QML access and QTest input on safe GUI-thread paths. |
| Screenshot | Watch base64 payload size and WebSocket max-size limits. |
| Log sink | Avoid duplicate sink registration and unbounded log history. |
| Engine | Preserve shader paths, post-build copy rules, and current working directory assumptions. |
| Render loop | Validate 3D rendering after changing `QT_OPENGL` or `QSG_RENDER_LOOP`. |
| Submodules | Update submodule commits and nested dependency notes together. |
| Docs | Update protocol, build, and run documentation with behavior changes. |

## Known Failure Modes

| Failure | Likely Cause |
|---|---|
| `Qt5::WebSockets` is missing | The selected Qt package is incomplete. |
| Debug app crashes | Mixed Qt runtime/build configurations can be unstable on the QFBO path. |
| Shader load failure | Working directory is wrong or post-build shader copy did not run. |
| Live shader compile failure | `TESTBRIDGE_SHADERC` is wrong, include paths are missing, or the stage/profile is invalid. |
| Live shader apply failure | Slot is not allowlisted, compiled hash is stale, or compute is unavailable in the current tier. |
| WebSocket disconnect | Single-client policy, idle timeout, or port conflict. |
| Screenshot timeout | Payload too large or window not fully rendered. |
| Clean clone build failure | Recursive submodules were not initialized. |

## Acceptance Criteria

1. Release build succeeds.
2. Real GUI smoke test passes.
3. MCP unit tests pass.
4. TestBridge is not tied to a product application.
5. New public behavior has documentation or tests.
6. Unverified cross-platform behavior is explicitly marked.
7. No local absolute paths are required by the documented workflow.
