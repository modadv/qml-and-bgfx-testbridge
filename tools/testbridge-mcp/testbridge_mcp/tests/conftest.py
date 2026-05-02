import asyncio
import json
import pytest
import websockets


@pytest.fixture
async def mock_bridge():
    """Spin up a minimal mock WebSocket server that handles app.ping and bus.publish."""
    events: list[dict] = []
    notify_queues: list[asyncio.Queue] = []

    async def handler(websocket):
        async for raw in websocket:
            msg = json.loads(raw)
            method = msg.get("method")
            req_id = msg.get("id")
            params = msg.get("params") or {}

            if method == "app.ping":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": "pong"
                }))
            elif method == "app.version":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": "0.0.0-test"
                }))
            elif method == "app.describe":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "applicationName": "mock-app",
                        "protocolVersion": "0.2.0",
                        "methods": ["app.describe", "qml.meta", "render.caps", "shader.compile"],
                        "windows": []
                    }
                }))
            elif method == "bus.publish":
                events.append(params)
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": None
                }))
                # Emit a bus.event notification for subscribers
                notification = {
                    "jsonrpc": "2.0",
                    "method": "bus.event",
                    "params": {"topic": params.get("topic"), "payload": params.get("payload")}
                }
                await websocket.send(json.dumps(notification))
            elif method == "bus.subscribe":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": None
                }))
            elif method == "bus.wait":
                topic = params.get("topic")
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"topic": topic, "payload": {"waited": True}}
                }))
            elif method == "qml.find":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": 12345
                }))
            elif method == "qml.meta":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "handle": params.get("handle"),
                        "class": "MockItem",
                        "properties": [{"name": "visible", "value": True}],
                        "methods": [],
                        "signals": []
                    }
                }))
            elif method in ("qml.click", "qml.type", "qml.set", "app.quit"):
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": None
                }))
            elif method in ("qml.get", "qml.invoke"):
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": "mock_value"
                }))
            elif method == "window.grab":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": "base64encodedpng=="
                }))
            elif method == "log.recent":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": ["log line 1", "log line 2"]
                }))
            elif method == "log.wait":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id, "result": ["matched line"]
                }))
            elif method == "render.caps":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"available": True, "backend": "bgfx", "tier": "full"}
                }))
            elif method == "render.stats":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"available": True, "currentFrame": {"updateMs": 1.0}}
                }))
            elif method == "render.resources":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"available": True, "heightfieldReady": True}
                }))
            elif method == "test.artifacts":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"available": True, "recommendedRpc": ["window.grab"]}
                }))
            elif method == "shader.compile":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "ok": True,
                        "hash": "abc123",
                        "slot": params.get("slot", "terrain_simple.fragment"),
                        "cached": False,
                        "stderr": ""
                    }
                }))
            elif method == "shader.apply":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"queued": True, "hash": params.get("hash")}
                }))
            elif method == "shader.revert":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {"queued": True, "slot": params.get("slot")}
                }))
            elif method == "shader.list":
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "result": {
                        "available": True,
                        "supportedSlots": ["terrain_simple.fragment"],
                        "records": [{"hash": "abc123", "ok": True}]
                    }
                }))
            else:
                await websocket.send(json.dumps({
                    "jsonrpc": "2.0", "id": req_id,
                    "error": {"code": -32601, "message": f"Method not found: {method}"}
                }))

    server = await websockets.serve(handler, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]

    yield {"host": "127.0.0.1", "port": port, "events": events}

    server.close()
    await server.wait_closed()
