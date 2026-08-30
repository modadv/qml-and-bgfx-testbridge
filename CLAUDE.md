# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Repo Is

`testbridge-lab` is an Agent-ready starter kit for Qt 5.15 QML + bgfx desktop
apps. Its point is the *automation loop*: the app embeds an in-process
WebSocket JSON-RPC server (TestBridge) so an agent can inspect the live QML
tree, drive input, read renderer state, hot-swap shaders, and screenshot the
real window — instead of inferring UI/render state from source.

**Do not guess QML object state or renderer state from source.** Build, launch
the app, and query it through the MCP tools in `tools/testbridge-mcp` or the
smoke tests.

## Build & Test

Windows Release is the primary validated path. Qt 5.15 (with `Qt5::WebSockets`)
must be supplied via `QT_PREFIX` / `CMAKE_PREFIX_PATH`.

```powershell
git submodule update --init --recursive   # bgfx.cmake + nested bgfx/bimg/bx
$env:QT_PREFIX = '<path-to-qt-5.15>'
conan install . -of .build-release -s build_type=Release -o '&:use_system_qt=True' -c user.build:system_qt_prefix="$env:QT_PREFIX" --build=missing
cmake -S . -B .build-release\build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=.build-release\build\generators\conan_toolchain.cmake -DCMAKE_PREFIX_PATH="$env:QT_PREFIX"
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```

Convenience wrappers: `scripts\bootstrap.ps1`, `scripts\build.ps1`,
`scripts\test.ps1 [-Label <ctest-label>]`, `scripts\run.ps1`,
`scripts\deploy.ps1`.

**Use explicit `-S`/`-B` commands, not `cmake --preset`.** `CMakeUserPresets.json`
includes conan generator presets from *both* `.build-release/build` and
`.build`, so preset reading currently fails with `Duplicate preset:
"conan-default"` whenever both dirs exist.

### Running the app

```powershell
scripts\run.ps1                              # Release, auto-discovered Qt
scripts\run.ps1 -Tier nocompute -Port 17777
scripts\deploy.ps1 -Clean                    # self-contained dist\testbridge_lab (~40 MB)
```

Launching `testbridge_lab.exe` directly fails, for two reasons that Visual
Studio papers over:

- **Qt is not on `PATH`.** Nothing copies Qt's DLLs next to the executable;
  `conan_toolchain.cmake` sets `CMAKE_VS_DEBUGGER_ENVIRONMENT`, which prepends
  the conan Qt bin dir *only for the VS debugger*. From any other shell the
  loader fails on `Qt5Core.dll`.
- **cwd must be the executable's own directory.** `main.cpp` loads
  `QDir::currentPath() + "/qml/Main.qml"`, and the bgfx loader resolves
  `shaders/<renderer>/*.bin` relative to cwd.

`scripts\run.ps1` does both. It resolves the Qt bin dir from
`$env:TESTBRIDGE_QT_BIN`, else from the conan generators (`conanrunenv-*.bat`,
`conan_toolchain.cmake`) — never hardcode the conan cache path, its package hash
changes on re-install. `scripts\qt_env.ps1` holds that discovery and is
dot-sourced by both scripts. Note the conan Qt package puts its plugins under
`bin\archdatadir\plugins`, not the usual `<prefix>\plugins`.

`scripts\deploy.ps1` runs `windeployqt` to produce a folder that needs no
environment at all. Qt is the only shared dependency to deploy — spdlog and fmt
are static here (their conan packages ship no `bin\`) and bgfx is linked in.

### Running individual tests

```powershell
ctest --test-dir .build-release\build -C Release -R testbridge_lab_smoke_full --output-on-failure
ctest --test-dir .build-release\build -C Release -L render          # labels: render, full, nocompute, shader, golden, template, package
# $py = the interpreter ctest itself uses (see below)
& $py tests\smoke_testbridge_lab.py                                  # smoke test standalone
& $py -m pytest tools\testbridge-mcp\testbridge_mcp\tests -q         # MCP unit tests (mock WS, no app)
& $py tools\template\validate_manifest.py
```

Run the Python tests with the *same* interpreter ctest resolved, or they test a
different environment. CMake picks it at configure time —
`tools\testbridge-mcp\.venv\Scripts\python.exe` if that exists, else
`find_package(Python3)` — and bakes the absolute path into every `add_test`
line, so read it back rather than guessing (`TESTBRIDGE_PYTHON` in `CMakeCache`
is the *override* knob and is normally empty):

```powershell
$py = ((Select-String -Path .build-release\build\CTestTestfile.cmake `
        -Pattern 'add_test\(testbridge_mcp_unit "([^"]+)"').Matches[0].Groups[1].Value)
```

Its deps are `fastmcp`, `websockets`, `pytest`, `pytest-asyncio`
(`tools\testbridge-mcp\pyproject.toml`).

Raw `ctest` needs `$env:TESTBRIDGE_QT_BIN` set, or every app-launching test dies
at startup with `0xC0000135` (STATUS_DLL_NOT_FOUND) and reports only "TestBridge
app exited before port N opened". `scripts\test.ps1` resolves and sets it for
you.

CTest registers 10 tests. Most of them are the *same* `smoke_testbridge_lab.py`
launched with different `ENVIRONMENT` properties (render tier, window size,
live-shader selftest) — see the `add_test` block in the root `CMakeLists.txt`.
`testbridge_lab_golden_probe` is registered `DISABLED` on purpose until a
project defines stable goldens.

Test behavior knobs (also useful when running the smoke test by hand):

```powershell
$env:TESTBRIDGE_LAB_APP / TESTBRIDGE_QT_BIN / TESTBRIDGE_PORT / TESTBRIDGE_ARTIFACT_DIR
$env:TESTBRIDGE_RENDER_TIER = 'full' | 'nocompute'   # nocompute = software/no-compute path
$env:TESTBRIDGE_LIVE_SHADER_SELFTEST = '1'
$env:TESTBRIDGE_SHADERC = '<path-to-shaderc.exe>'    # built under .build-release\build\external\bgfx.cmake\...
$env:TESTBRIDGE_UPDATE_GOLDEN = '1'                  # only for intended visual changes
```

A silent startup failure with an empty log and "TestBridge port did not open"
is almost always missing Qt DLLs or plugin paths, not a code bug — see "Running
the app" above.

## Architecture

Three layers, deliberately decoupled — the seam between them is the thing to
understand before making changes.

| Layer | Target | Role |
|---|---|---|
| `src/app` | `testbridge_lab` | Host: sets GL/render-loop env, registers `RenderViewportItem` in QML, loads `qml/Main.qml`, starts TestBridge, and **wires the engine to the bridge**. |
| `src/testbridge` | `testbridge` | Transport + protocol only. Knows nothing about bgfx. |
| `src/engine` | `engine_core`, `engine_terrain_assets`, `engine_shaders` | bgfx terrain renderer + Qt Quick adapter. Knows nothing about the bridge. |

`src/core/logger`, `src/quick` (`GuiDataBus`), and `src/config` are reference-host shims.

### The app is the wiring layer

TestBridge exposes render/shader RPCs but has no engine dependency. Instead
`src/app/main.cpp` injects `JsonProvider` callbacks
(`bridge.setRenderCapsProvider`, `setRenderStatsProvider`,
`setRenderResourcesProvider`, `setShaderCompile/Apply/Revert/ListHandler`) that
snapshot engine state into JSON. **To expose new renderer state to agents you
change two places**: the engine snapshot producer, and the provider lambda in
`main.cpp`. See `docs/RENDER_PROVIDER_SDK.md`.

### Render path

Qt Quick → bgfx goes through `QQuickFramebufferObject`:

1. `RenderViewportItem` (QFBO) syncs QML properties into `RenderScene`.
2. `RenderScene` owns camera/overlay/input/renderer state and presents through
   the thin `IRenderContent` interface (`quick/render_content.h`) — the present
   path is *not* welded to `TerrainRenderer`.
3. `ReadbackPresenter` owns the `ViewSurface`, the readback ring, and the
   readback → `glTexSubImage2D` upload into the Qt FBO texture.
4. `RenderDevice` owns the global bgfx lifecycle, views, render targets, and the
   deferred-delete queue.

This readback/upload present is portable and test-friendly but costly; it is the
known perf tradeoff of the template.

### Render-path discipline (non-negotiable)

- Destroy every bgfx resource through the `DeferredDeleteQueue`
  (`src/engine/common/resource_arena.h`): `enqueue(destroy, safeAfterFrame)`.
  Never a hand-counted "wait N frames" delay, never a bare `bgfx::destroy` on a
  resource that may still be in flight.
- Keep the loop render-on-demand. Signal continued frames via
  `IRenderContent::needsContinuousUpdate()`; do not unconditionally `update()`.
- Guard programs/handles with `bgfx::isValid` before `submit`/`dispatch`.
- `engine::FramePassList` (`terrain/frame_graph.h`) is a *declarative
  description* of pass topology with read/write sets, validated for
  producer-before-consumer ordering at init and reported under
  `render.resources.framePasses`. It is not a scheduler — no DAG, aliasing, or
  barriers. Adding a pass means adding it here too.
- `TerrainRenderer` is one class split across
  `terrain_renderer{,_simple,_overlay,_heightfield,_shaders}.cpp`, with all
  member declarations in `terrain_renderer.h`. Add a method to the `.cpp`
  matching its concern (`_simple` = NoCompute grid + CPU smap, `_overlay` =
  rects/picking/overlay-max readback, `_heightfield` = load/decode/cache,
  `_shaders` = program load + live reload).

Rationale: `docs/ARCHITECTURE.md` § "Render-path discipline".

### Shaders

`src/engine/shaders/*.sc` are compiled by `engine_shaders` via bgfx's `shaderc`
into `dx11`/`glsl`/`spirv` (Windows). Shader lists live in
`src/engine/CMakeLists.txt` — adding a shader means adding its name to
`VERTEX_SHADERS`/`FRAGMENT_SHADERS`/`COMPUTE_SHADERS`. A post-build step copies
`qml/` and `shaders/` next to the executable because the bgfx loader resolves
`shaders/<renderer>/*.bin` relative to cwd.

## Conventions

- Every testable QML item needs a stable, non-generated `objectName`. Existing
  hooks: `main_window`, `lab_increment_click`, `lab_reset_click`,
  `lab_counter_label`, `lab_status_label`, `lab_engine_view_3d`.
- Expose app state through `Q_PROPERTY`, `Q_INVOKABLE`, logs, or TestBridge RPCs.
- **TestBridge RPC handlers must never call bgfx directly.** Copy renderer state
  into JSON snapshots at safe frame boundaries; queue mutations (like live
  shader apply) to the render thread.
- Live shaders only through allowlisted slots and the
  compile → cache → apply → revert path. Sample slots:
  `terrain_simple.vertex`, `terrain_simple.fragment`,
  `overlay_max_elevation.compute` (compute tier only).
- Add render resource counters when adding textures, buffers, programs, or passes.
- Release builds should be able to run with `ENABLE_TESTBRIDGE=OFF`
  (CMake options: `ENABLE_TESTBRIDGE`, `ENABLE_LIVE_SHADER`, `ENABLE_AGENT_MCP`,
  `ENABLE_RENDER_GOLDEN_TESTS`).
- Keep `external/bgfx.cmake` a submodule gitlink; never vendor its sources into
  the main index.

## Working With A Live App

Start every inspection session with `app.describe`, `qml.tree`, `render.caps`,
`render.stats`, `render.resources`, `window.grab`.

- UI work: `qml.find` → `qml.meta`/`qml.geometry` → `qml.click`/`qml.mouse`/`qml.key`/`qml.invoke` → `qml.get` + logs + screenshot.
- Render work: add the field to `render.resources`/`render.stats` *before*
  writing a test that needs it; verify backend support with `render.caps`.
- Shader work: `shader.list` → `shader.compile` → `shader.apply` → verify via
  `render.resources.liveShader` + `window.grab` → `shader.revert`.

On failure, read the artifact dir printed by the test in this order:
`manifest.json`, `app.log`, `screenshot.png`, then
`actual_crop.png`/`expected.png`/`diff.png`/`metrics.json`. **Preserve artifacts**
— they are the next session's debugging context.

TestBridge is loopback-only with no authentication. Never expose the port.

## Docs

`AGENTS.md` and `docs/AGENT_RUNBOOK.md` first. Then `docs/ARCHITECTURE.md`
(module boundaries, RPC surface, risks), `docs/AGENT_WORKFLOWS.md` (repeatable
loops), `docs/PROJECT_CONVENTIONS.md`, `docs/RENDER_PROVIDER_SDK.md`,
`docs/RPC_PROTOCOL.md`, `docs/TEMPLATE_USAGE.md` (spawn a new project via
`py -3 scripts\new_project.py "<Name>" <dest>`).
