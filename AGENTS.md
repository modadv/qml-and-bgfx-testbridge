# Agent Guide

This repository is a starter kit for Qt 5.15 QML + bgfx desktop apps that are
designed to be developed and tested by Coding Agents.

## First Commands

```powershell
cmake -S . -B .build-release\build
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```

Use `tools\testbridge-mcp\.venv\Scripts\python.exe` when the MCP virtualenv is
available. The CTest suite launches the real app and verifies QML, bgfx, live
shader, screenshot, and render-state paths.

## Agent Rules

- Keep QML `objectName` stable for every testable control.
- Expose app state through `Q_PROPERTY`, `Q_INVOKABLE`, logs, or TestBridge RPCs.
- Do not call bgfx directly from TestBridge RPC threads.
- Use render providers and snapshots for engine state.
- Use `shader.compile/apply/revert` only for whitelisted live shader slots.
- On visual changes, run smoke tests and update goldens only when the change is intended.
- Preserve failure artifacts; they are the next Agent's debugging context.

## Key Workflows

- UI work: `qml.find`, `qml.meta`, `qml.geometry`, `qml.tree`, `qml.click`.
- Render work: `render.caps`, `render.stats`, `render.resources`, `window.grab`.
- Shader work: `shader.compile`, `shader.apply`, screenshot/golden diff, `shader.revert`.
- Failure work: inspect `artifacts/*/manifest.json`, screenshot, app log, and diff images.

Read [docs/AGENT_WORKFLOWS.md](docs/AGENT_WORKFLOWS.md) before making broad changes.
