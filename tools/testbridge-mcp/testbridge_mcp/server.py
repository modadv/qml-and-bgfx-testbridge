import os
from typing import Any

from fastmcp import FastMCP

from .ws_client import WSClient

mcp = FastMCP("testbridge-mcp")

_client: WSClient | None = None


def _get_client() -> WSClient:
    global _client
    if _client is None:
        host = os.environ.get("TESTBRIDGE_HOST", "127.0.0.1")
        port = int(os.environ.get("TESTBRIDGE_PORT", "47600"))
        _client = WSClient(host=host, port=port)
    return _client


@mcp.tool()
async def app_ping() -> dict:
    """Ping the TestBridge and return version info."""
    pong = await _get_client().call("app.ping")
    version = await _get_client().call("app.version")
    return {"status": "ok", "result": pong, "version": version}


@mcp.tool()
async def app_describe() -> dict:
    """Return application, protocol, window and supported RPC metadata."""
    result = await _get_client().call("app.describe")
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def app_quit() -> None:
    """Tell the application to quit."""
    await _get_client().call("app.quit")


@mcp.tool()
async def bus_publish(topic: str, payload: Any = None) -> None:
    """Publish an event onto the GuiDataBus."""
    await _get_client().call("bus.publish", {"topic": topic, "payload": payload})


@mcp.tool()
async def bus_wait_for(topic: str, timeout_ms: int = 5000) -> dict:
    """Block until a GuiDataBus event arrives on the given topic."""
    result = await _get_client().call("bus.wait", {"topic": topic, "timeout_ms": timeout_ms},
                                      timeout=timeout_ms / 1000.0 + 5.0)
    return result if isinstance(result, dict) else {"payload": result}


@mcp.tool()
async def qml_find(object_name: str) -> int:
    """Find a QML object by objectName and return its handle."""
    result = await _get_client().call("qml.find", {"objectName": object_name})
    return int(result)


@mcp.tool()
async def qml_get(handle: int, prop: str) -> Any:
    """Get a QML property value."""
    return await _get_client().call("qml.get", {"handle": handle, "property": prop})


@mcp.tool()
async def qml_set(handle: int, prop: str, value: Any) -> None:
    """Set a QML property value."""
    await _get_client().call("qml.set", {"handle": handle, "property": prop, "value": value})


@mcp.tool()
async def qml_invoke(handle: int, method: str, args: list[Any] = []) -> Any:
    """Invoke a QML method and return the result."""
    return await _get_client().call("qml.invoke", {"handle": handle, "method": method, "args": args})


@mcp.tool()
async def qml_click(handle: int) -> None:
    """Simulate a mouse click on a QML item."""
    await _get_client().call("qml.click", {"handle": handle})


@mcp.tool()
async def qml_type(handle: int, text: str) -> None:
    """Simulate keyboard typing into a QML item."""
    await _get_client().call("qml.type", {"handle": handle, "text": text})


@mcp.tool()
async def qml_geometry(handle: int) -> dict:
    """Return geometry and visual state of a QQuickItem.

    Fields:
      isQuickItem (bool), local{x,y,w,h}, scene{x,y,w,h},
      visible, effectivelyVisible (visible chain + opacity>0),
      enabled, opacity, z, rotation, scale, clip, activeFocus.
    Use scene{} for cross-component overlap / hit-test calculations.
    """
    result = await _get_client().call("qml.geometry", {"handle": handle})
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def qml_meta(handle: int, include_values: bool = True) -> dict:
    """Return QObject/QML meta-object data: properties, methods and signals."""
    result = await _get_client().call(
        "qml.meta",
        {"handle": handle, "include_values": include_values},
    )
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def qml_mouse(
    handle: int,
    action: str = "click",
    button: str = "left",
    modifiers: list[str] | None = None,
    delay_ms: int = -1,
) -> None:
    """Inject a mouse event onto a QQuickItem.

    action:    "click" | "dblclick" | "press" | "release" | "move"
    button:    "left" | "right" | "middle"
    modifiers: subset of ["ctrl","shift","alt","meta","keypad"]
    delay_ms:  optional delay between press and release (-1 = QTest default)
    """
    params: dict[str, Any] = {"handle": handle, "action": action, "button": button}
    if modifiers is not None:
        params["modifiers"] = modifiers
    if delay_ms >= 0:
        params["delay_ms"] = delay_ms
    await _get_client().call("qml.mouse", params)


@mcp.tool()
async def qml_key(
    handle: int,
    key: str,
    action: str = "click",
    modifiers: list[str] | None = None,
    delay_ms: int = -1,
) -> None:
    """Inject a key event onto a QQuickItem.

    key:       "enter" | "escape" | "tab" | "space" | "backspace" | "delete" |
               "home"  | "end"    | "pageup" | "pagedown" |
               "up" | "down" | "left" | "right" |
               "f1"..."f12", or any single character ("a", "5").
    action:    "click" | "press" | "release"
    modifiers: subset of ["ctrl","shift","alt","meta","keypad"]
    delay_ms:  optional delay (-1 = QTest default)
    """
    params: dict[str, Any] = {"handle": handle, "key": key, "action": action}
    if modifiers is not None:
        params["modifiers"] = modifiers
    if delay_ms >= 0:
        params["delay_ms"] = delay_ms
    await _get_client().call("qml.key", params)


@mcp.tool()
async def qml_tree(max_depth: int = 30, only_visible: bool = True, max_nodes: int = 5000) -> dict:
    """Dump the live QML render tree from each QQuickWindow's contentItem.

    Returns {items: [...], count, truncated, limit}. Each item has: class,
    objectName, depth, scene{x,y,w,h}, visible, opacity, clip. Walks
    QQuickItem::childItems(). When truncated=true the response was capped at
    max_nodes — raise the limit or narrow the depth.

    Use this when an on-screen control isn't reachable via qml.find/list —
    qml.tree shows everything that's actually rendered, with or without an objectName.
    """
    result = await _get_client().call(
        "qml.tree",
        {"max_depth": max_depth, "only_visible": only_visible, "max_nodes": max_nodes},
    )
    return result if isinstance(result, dict) else {"items": result, "count": len(result) if isinstance(result, list) else 0, "truncated": False}


@mcp.tool()
async def qml_hit(x: float, y: float) -> list:
    """Recursive hit-test: return the QQuickItem chain at scene point (x,y).

    The returned list goes from the deepest hit item up to the window root.
    Each entry has: class, objectName, handle, scene{x,y,w,h}, visible.
    Pair with qml.tree to identify which QML component renders a given screen region.
    """
    result = await _get_client().call("qml.hit", {"x": x, "y": y})
    return result if isinstance(result, list) else [result]


@mcp.tool()
async def screenshot(window_name: str | None = None) -> str:
    """Grab a screenshot and return it as base64-encoded PNG."""
    params: dict[str, Any] = {}
    if window_name:
        params["windowObjectName"] = window_name
    result = await _get_client().call("window.grab", params if params else None)
    return str(result)


@mcp.tool()
async def log_grep(regex: str, max_lines: int = 200) -> list[str]:
    """Return recent log lines matching a regex."""
    result = await _get_client().call("log.recent", {"max_lines": max_lines, "regex": regex})
    if isinstance(result, list):
        return [item.get("text", str(item)) if isinstance(item, dict) else str(item) for item in result]
    if isinstance(result, dict):
        return [str(result.get("text", result))]
    return [str(result)]


@mcp.tool()
async def log_wait(regex: str, timeout_ms: int = 10000) -> list[str]:
    """Block until a log line matching regex appears."""
    result = await _get_client().call("log.wait", {"regex": regex, "timeout_ms": timeout_ms},
                                      timeout=timeout_ms / 1000.0 + 5.0)
    if isinstance(result, list):
        return [item.get("text", str(item)) if isinstance(item, dict) else str(item) for item in result]
    if isinstance(result, dict):
        return [str(result.get("text", result))]
    return [str(result)]


@mcp.tool()
async def render_caps() -> dict:
    """Return host-provided bgfx renderer capabilities."""
    result = await _get_client().call("render.caps")
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def render_stats() -> dict:
    """Return host-provided renderer performance counters and frame stats."""
    result = await _get_client().call("render.stats")
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def render_resources() -> dict:
    """Return host-provided renderer resource and scene state."""
    result = await _get_client().call("render.resources")
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def test_artifacts() -> dict:
    """Return artifact collection capabilities and recommended diagnostic RPCs."""
    result = await _get_client().call("test.artifacts")
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def shader_compile(source: str, slot: str = "terrain_simple.fragment") -> dict:
    """Compile live shader source through the host shaderc pipeline."""
    result = await _get_client().call("shader.compile", {"slot": slot, "source": source}, timeout=40.0)
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def shader_apply(hash: str, slot: str = "terrain_simple.fragment") -> dict:
    """Queue a compiled live shader for safe application on the render thread."""
    result = await _get_client().call("shader.apply", {"slot": slot, "hash": hash})
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def shader_revert(slot: str = "terrain_simple.fragment") -> dict:
    """Revert a live shader slot to its stock program."""
    result = await _get_client().call("shader.revert", {"slot": slot})
    return result if isinstance(result, dict) else {"raw": result}


@mcp.tool()
async def shader_list() -> dict:
    """List live shader slots, cached compiles, and last applied hash."""
    result = await _get_client().call("shader.list")
    return result if isinstance(result, dict) else {"raw": result}


def run_stdio(host: str = "127.0.0.1", port: int = 47600) -> None:
    global _client
    _client = WSClient(host=host, port=port)
    mcp.run(transport="stdio")
