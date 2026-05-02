# Qt QML + bgfx Agent-Ready Template

This template initializes a desktop app with:

- Qt 5.15 QML shell
- bgfx offscreen render surface
- TestBridge JSON-RPC automation endpoint
- MCP tools for Coding Agents
- real GUI smoke tests
- render matrix tests
- golden-image oracle
- live shader compile/apply/revert loop

## Instantiate

```powershell
py -3 scripts\new_project.py "My Render App" <workspace>\my-render-app
```

## Verify

```powershell
git submodule update --init --recursive
cmake -S . -B .build-release\build
cmake --build .build-release\build --config Release --target my_render_app -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```

## Required Hooks

- Main window object name: `main_window`
- Primary bgfx render item object name: `lab_engine_view_3d` until renamed in tests
- All testable controls must have stable `objectName`
- Renderer must expose `render.caps`, `render.stats`, `render.resources`
- Live shader slots must be explicitly whitelisted

See `docs/AGENT_RUNBOOK.md` for unattended operation and `recipes/` for Agent
task workflows.
