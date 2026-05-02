# Agent Runbook

This runbook is the first document an unattended Coding Agent should read after
cloning a project generated from this template.

## Goal

Use TestBridge to develop and test a Qt 5.15 QML + bgfx desktop app without
manual UI inspection. The Agent should treat TestBridge data as the primary
source of truth and screenshots as visual evidence.

## Fresh Clone Bootstrap

```powershell
git submodule update --init --recursive
```

Expected submodule pins:

| Path | Commit |
|---|---|
| `external/bgfx.cmake` | `4e42ca1ef501a1e29d25975d735198fa5fad0903` |
| `external/bgfx.cmake/bgfx` | `6f36b4fb3a0d76090eb2727ecf11abac46eef8aa` |
| `external/bgfx.cmake/bimg` | `7afa2419254fd466c013a51bdeb0bee3022619c4` |
| `external/bgfx.cmake/bx` | `fa1411e4aa111c8b004c97660ab31ba1a5287835` |

Do not stage source files under `external/bgfx.cmake` in the main repository.
The main index should contain a `160000` gitlink for the submodule.

## Build

Use the repository's configured preset/build directory when available. The
generic Windows Release flow is:

```powershell
$env:QT_PREFIX = "<path-to-qt-5.15>"
conan install . -of .build-release -s build_type=Release -o '&:use_system_qt=True' -c user.build:system_qt_prefix="$env:QT_PREFIX" --build=missing
cmake -S . -B .build-release\build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=.build-release\build\generators\conan_toolchain.cmake -DCMAKE_PREFIX_PATH="$env:QT_PREFIX"
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
```

## Complete Verification

```powershell
ctest --test-dir .build-release\build -C Release --output-on-failure
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tools\testbridge-mcp\testbridge_mcp\tests -q
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tests\test_template_manifest.py -q
tools\testbridge-mcp\.venv\Scripts\python.exe tools\template\validate_manifest.py
```

Expected CTest coverage:

| Test | Purpose |
|---|---|
| `testbridge_lab_smoke` | Default real GUI TestBridge path. |
| `testbridge_lab_smoke_full` | Full render tier. |
| `testbridge_lab_smoke_nocompute_small` | NoCompute render tier and small window. |
| `testbridge_lab_live_shader_full` | Vertex, fragment, and compute live shader apply/revert where compute is available. |
| `testbridge_lab_live_shader_nocompute` | Vertex and fragment live shader apply/revert in NoCompute mode. |
| `testbridge_package_consumer` | Installed CMake package consumption. |
| `engine_heightfield_asset_tests` | Heightfield asset loading rules. |

`testbridge_lab_golden_probe` is intentionally registered as disabled until a
project defines stable golden baselines.

## Run A Manual TestBridge Session

```powershell
$env:QT_PREFIX = "<path-to-qt-5.15>"
$env:PATH = "$env:QT_PREFIX\bin;" + $env:PATH
$env:TESTBRIDGE_PORT = "47670"
.\.build-release\build\bin\Release\testbridge_lab.exe
```

Connect an MCP client to `127.0.0.1:47670`, or use raw JSON-RPC over WebSocket.
Start every session with:

1. `app.describe`
2. `qml.tree`
3. `render.caps`
4. `render.stats`
5. `render.resources`
6. `window.grab`

## Development Workflow

For UI work:

1. Add stable `objectName` values for every testable control.
2. Read the control through `qml.find`, `qml.meta`, and `qml.geometry`.
3. Trigger behavior through `qml.click`, `qml.mouse`, `qml.key`, or `qml.invoke`.
4. Assert state through `qml.get`, logs, and screenshots.

For render work:

1. Add state fields to `render.resources` or `render.stats` before writing tests
   that need them.
2. Verify hardware/backend capabilities with `render.caps`.
3. Use `window.grab` for actual visual evidence.
4. Use pixel heuristics or golden images for visual acceptance.

For shader work:

1. Discover slots with `shader.list`.
2. Compile source with `shader.compile`.
3. Apply the returned hash with `shader.apply`.
4. Verify `render.resources.liveShader.slots`, `window.grab`, and render stats.
5. Revert with `shader.revert` after experiments or failed visual checks.

Supported sample slots:

| Slot | Stage |
|---|---|
| `terrain_simple.vertex` | Vertex |
| `terrain_simple.fragment` | Fragment |
| `overlay_max_elevation.compute` | Compute |

Hosts must explicitly allowlist their own live shader slots.

## Failure Triage

When a smoke test fails, inspect the artifact directory printed by the test.
Read files in this order:

1. `manifest.json`
2. `app.log`
3. `screenshot.png`
4. `actual_crop.png`, `expected.png`, `diff.png`, and `metrics.json` when golden
   comparison is active

Common failure classes:

| Symptom | First Checks |
|---|---|
| TestBridge port never opens | App crash, Qt runtime path, port conflict, `ENABLE_TESTBRIDGE`. |
| QML object not found | Missing or renamed `objectName`, invisible loader state, wrong screen. |
| Input has no effect | Item disabled/hidden, hit target covered, focus problem, wrong event type. |
| Screenshot blank | Window not visible, render loop not advanced, bgfx init failure. |
| Shader compile fails | `TESTBRIDGE_SHADERC`, include path, stage/profile mismatch, shader syntax. |
| Shader apply fails | Unsupported slot, stale hash, render tier lacks compute. |
| Render resources invalid | Shader copy path, bgfx generation reset, asset path failure. |
| Golden diff fails | Intended visual change, unstable threshold, backend/platform difference. |

## Acceptance Criteria

A change is ready for handoff when:

1. Release build passes.
2. Full CTest passes except intentionally disabled tests.
3. MCP unit tests pass.
4. Template manifest validation passes.
5. Failure artifacts are generated for forced smoke failures.
6. Public behavior changes are reflected in docs and tests.
7. `external/bgfx.cmake` remains a submodule gitlink, not vendored source.

## Safety Boundaries

- TestBridge is for loopback development and CI only.
- Production builds should normally use `ENABLE_TESTBRIDGE=OFF`.
- RPC handlers must not mutate bgfx directly from arbitrary RPC threads.
- Live shaders must use allowlisted slots and the controlled
  compile/cache/apply/revert path.
- Shader include paths should stay project-owned and explicit.
- Agents should preserve artifacts because they are the next debugging context.
