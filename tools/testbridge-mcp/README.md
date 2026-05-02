# testbridge-mcp

MCP server that bridges AI coding agents to **TestBridge** via WebSocket
JSON-RPC 2.0. It is intended for projects generated from the Qt QML + bgfx
Agent template as well as apps that embed the TestBridge runtime directly.

## Quick Start

### 1. Install

```powershell
cd tools/testbridge-mcp
py -3 -m venv .venv
.\.venv\Scripts\python.exe -m pip install -e .[dev]
```

### 2. Start testbridge-lab

```powershell
$env:QT_PREFIX = '<path-to-qt>'
$env:PATH = "$env:QT_PREFIX\bin;" + $env:PATH
$env:TESTBRIDGE_PORT = '47670'
cd <repo>
.\.build-release\build\bin\Release\testbridge_lab.exe
```

### 3. Run the MCP server

```powershell
tools\testbridge-mcp\.venv\Scripts\testbridge-mcp.exe --host 127.0.0.1 --port 47670
```

Or via module:

```powershell
tools\testbridge-mcp\.venv\Scripts\python.exe -m testbridge_mcp --host 127.0.0.1 --port 47670
```

Environment variables override defaults:
- `TESTBRIDGE_HOST` (default `127.0.0.1`)
- `TESTBRIDGE_PORT` (default `47600`)

---

## Wiring into Claude Code

Add to your project's `.claude/mcp.json` (or `~/.claude/mcp.json`):

```json
{
  "mcpServers": {
    "testbridge": {
      "command": "/path/to/.venv/bin/testbridge-mcp",
      "args": ["--host", "127.0.0.1", "--port", "47600"]
    }
  }
}
```

## Wiring into Codex CLI

```json
{
  "mcpServers": {
    "testbridge": {
      "command": "testbridge-mcp",
      "args": []
    }
  }
}
```

## Wiring into Gemini CLI

```json
{
  "tools": [
    {
      "type": "mcp",
      "server": {
        "command": "testbridge-mcp",
        "args": ["--port", "47600"]
      }
    }
  ]
}
```

---

## Available MCP Tools

| Tool | RPC | Description |
|---|---|---|
| `app_ping` | `app.ping` | Ping app; returns version info |
| `app_describe` | `app.describe` | Return app/protocol/window/RPC metadata |
| `app_quit` | `app.quit` | Tell app to quit |
| `bus_publish(topic, payload)` | `bus.publish` | Publish in-process event bus message |
| `bus_wait_for(topic, timeout_ms)` | `bus.wait` | Block until event arrives |
| `qml_find(object_name)` | `qml.find` | Find QML object and return handle int |
| `qml_get(handle, prop)` | `qml.get` | Read QML property |
| `qml_set(handle, prop, value)` | `qml.set` | Write QML property |
| `qml_invoke(handle, method, args)` | `qml.invoke` | Call QML method |
| `qml_click(handle)` | `qml.click` | Simulate mouse click |
| `qml_type(handle, text)` | `qml.type` | Simulate keyboard input |
| `qml_geometry(handle)` | `qml.geometry` | Return QQuickItem geometry and visual state |
| `qml_meta(handle, include_values)` | `qml.meta` | Return QObject properties, methods, and signals |
| `qml_mouse(handle, action, button, modifiers, delay_ms)` | `qml.mouse` | Inject mouse event |
| `qml_key(handle, key, action, modifiers, delay_ms)` | `qml.key` | Inject key event |
| `qml_tree(max_depth, only_visible, max_nodes)` | `qml.tree` | Dump rendered QQuickItem tree |
| `qml_hit(x, y)` | `qml.hit` | Hit-test rendered QQuickItems at scene coordinates |
| `screenshot(window_name?)` | `window.grab` | Grab screenshot as base64 PNG |
| `log_grep(regex, max_lines)` | `log.recent` | Search recent log lines |
| `log_wait(regex, timeout_ms)` | `log.wait` | Block until log line matches |
| `render_caps()` | `render.caps` | Return bgfx renderer capabilities |
| `render_stats()` | `render.stats` | Return renderer performance counters |
| `render_resources()` | `render.resources` | Return renderer resource/scene state |
| `test_artifacts()` | `test.artifacts` | Return diagnostic artifact collection capabilities |
| `shader_compile(source, slot)` | `shader.compile` | Compile live shader source through shaderc |
| `shader_apply(hash, slot)` | `shader.apply` | Queue compiled shader application |
| `shader_revert(slot)` | `shader.revert` | Revert live shader slot to stock program |
| `shader_list()` | `shader.list` | List supported live shader slots and cache records |

Sample live shader slots exposed by this template:

| Slot | Stage |
|---|---|
| `terrain_simple.vertex` | Vertex |
| `terrain_simple.fragment` | Fragment |
| `overlay_max_elevation.compute` | Compute |

Always call `shader_list()` before compiling. Generated host applications may
rename, add, or remove slots.

---

## Agent Recipes

Generated projects include reusable recipes under
`templates/qt-qml-bgfx/recipes/`. Start with:

- `add-qml-control-and-test.md` for UI feature work.
- `add-render-provider-field.md` for engine telemetry.
- `live-shader-iteration.md` for controlled shader compile/apply/revert loops.
- `golden-image-oracle.md` for screenshot and render-region comparisons.

## Running Tests

```powershell
cd <repo>
tools\testbridge-mcp\.venv\Scripts\python.exe -m pytest tools\testbridge-mcp\testbridge_mcp\tests -q
```

All tests use an in-process mock WebSocket server — no running app required.
