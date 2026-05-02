# TestBridge Lab

Qt Quick + bgfx starter kit used to create Agent-ready desktop graphics apps
with a built-in local automation and render-inspection loop.

## What Is Included

- `src/testbridge`: migrated in-process WebSocket JSON-RPC TestBridge.
- `src/engine`: migrated bgfx terrain renderer and Qt Quick item.
- `src/app`: minimal host app with a QML UI, a bgfx-backed 3D area, and stable
  `objectName` hooks for automation.
- `tools/testbridge-mcp`: Python MCP bridge for Codex, Claude Code, and other
  Agent tools.
- `templates/qt-qml-bgfx`: template manifest, recipes, and validation schema.
- `tests/smoke_testbridge_lab.py`: real app smoke test through TestBridge.

## Documentation

- [Agent Runbook](docs/AGENT_RUNBOOK.md): first-read bootstrap, verification,
  TestBridge usage, failure triage, and safety rules for unattended Agents.
- [Architecture](docs/ARCHITECTURE.md): current module boundaries, runtime flow,
  protocol surface, build graph, and known risks.
- [Template Usage](docs/TEMPLATE_USAGE.md): create a new Qt QML + bgfx project
  from this starter kit.
- [Agent Workflows](docs/AGENT_WORKFLOWS.md): repeatable coding, UI, render,
  shader, artifact, and release loops for autonomous Agents.
- [Project Conventions](docs/PROJECT_CONVENTIONS.md): naming, QML hooks,
  render contracts, and testing rules for generated projects.
- [Protocol Schema](docs/PROTOCOL_SCHEMA.md): JSON-RPC and MCP tool contract.
- [RPC Protocol](docs/RPC_PROTOCOL.md): runtime RPC groups exposed by
  TestBridge.
- [QML Agent Hooks](docs/QML_AGENT_HOOKS.md): object naming and QML
  introspection rules.
- [bgfx Agent Contract](docs/BGFX_AGENT_CONTRACT.md): render capabilities,
  stats, resources, screenshots, and live shader expectations.
- [Render Provider SDK](docs/RENDER_PROVIDER_SDK.md): how an engine exposes
  caps, stats, resources, screenshots, and live shader hooks.
- [Pluggable Framework Direction](docs/PLUGIN_FRAMEWORK.md): host integration
  contract, agent-facing capabilities, and test tiers.
- [Agent Integration](docs/AGENT_INTEGRATION.md): CMake package integration,
  required host hooks, introspection RPCs, MCP tools, and artifact workflow.
- [CI Matrix](docs/CI_MATRIX.md): render-tier, window-size, and platform matrix
  used to move Agent validation closer to unattended operation.
- [Live Shader Direction](docs/LIVE_SHADER_DIRECTION.md): recommended design for
  Agent-driven shader iteration on top of bgfx.
- [Validation Checklist](docs/VALIDATION_CHECKLIST.md): commands and criteria for
  validating changes.
- [Technical Decisions](docs/DECISIONS.md): extraction-stage decisions and
  tradeoffs.

## Build

This Windows lab expects Qt 5.15 with `Qt5::WebSockets`. Configure the Qt
installation through `QT_PREFIX` or the matching CMake/Conan cache variable.

To initialize a new project from the template:

```powershell
py -3 scripts\new_project.py "Terrain Studio" <workspace>\terrain-studio
```

Initialize submodules before the first build:

```powershell
git submodule update --init --recursive
```

```powershell
$env:QT_PREFIX = '<path-to-qt>'
conan install . -of .build-release -s build_type=Release -o '&:use_system_qt=True' -c user.build:system_qt_prefix="$env:QT_PREFIX" --build=missing
cmake -S . -B .build-release\build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE=.build-release\build\generators\conan_toolchain.cmake -DCMAKE_PREFIX_PATH="$env:QT_PREFIX"
cmake --build .build-release/build --config Release --target testbridge_lab -- /m
```

## Run

```powershell
$env:QT_PREFIX = '<path-to-qt>'
$env:PATH = "$env:QT_PREFIX\bin;" + $env:PATH
$env:TESTBRIDGE_PORT = '47670'
cd .build-release\build\bin\Release
.\testbridge_lab.exe
```

## Verify

```powershell
ctest --test-dir .build-release\build -C Release --output-on-failure
tools\testbridge-mcp\.venv\Scripts\python.exe tests\smoke_testbridge_lab.py
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tests -q
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tools\testbridge-mcp\testbridge_mcp\tests -q
tools\testbridge-mcp\.venv\Scripts\python.exe tools\template\validate_manifest.py
```

The smoke test launches the real app, connects to TestBridge, verifies
`app.ping/app.version`, enumerates and hit-tests QML objects, triggers
`qml.click`, `qml.mouse`, and `qml.key` input, reads `lab_counter_label`, waits
for a log line, grabs a PNG screenshot, and quits the app via RPC. It also
verifies `app.describe`, `qml.meta`, `qml.geometry`, `qml.tree`, `render.caps`,
`render.stats`, `render.resources`, live shader state, and that the bgfx render
region contains varied pixels, not just a PNG header.

When `TESTBRIDGE_LIVE_SHADER_SELFTEST=1`, the live shader smoke path compiles,
applies, observes, and reverts the allowlisted sample slots:
`terrain_simple.vertex`, `terrain_simple.fragment`, and
`overlay_max_elevation.compute` when the selected render tier supports compute.

Useful environment overrides:

```powershell
$env:TESTBRIDGE_LAB_APP = '<path-to-testbridge_lab.exe>'
$env:TESTBRIDGE_QT_BIN = '<path-to-qt-bin>'
$env:TESTBRIDGE_CONNECT_TIMEOUT = '10'
$env:TESTBRIDGE_RENDER_TIER = 'full'       # or 'nocompute'
$env:TESTBRIDGE_ARTIFACT_DIR = '<artifact-dir>'
$env:TESTBRIDGE_LIVE_SHADER_SELFTEST = '1'
$env:TESTBRIDGE_SHADERC = '<path-to-shaderc.exe>'
```

## Current Notes

- Release is the primary validated local build path.
- Mixing Debug application builds with a Release Qt runtime can be unstable.
  Prefer matching Qt/runtime build types when investigating renderer startup.
- `QT_OPENGL=desktop` and `QSG_RENDER_LOOP=basic` are set by `main.cpp` before
  `QGuiApplication` to keep `QQuickFramebufferObject + bgfx` stable.
- Compiled shaders are copied to the executable directory after build because
  the migrated bgfx loader resolves `shaders/<renderer>/*.bin` relative to cwd.
