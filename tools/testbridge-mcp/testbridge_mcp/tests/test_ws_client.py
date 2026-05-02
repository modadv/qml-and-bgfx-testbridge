import asyncio
import json
import pytest
import websockets

from testbridge_mcp.ws_client import WSClient, TestBridgeError


@pytest.mark.asyncio
async def test_connect_and_ping(mock_bridge):
    client = WSClient(mock_bridge["host"], mock_bridge["port"])
    result = await client.call("app.ping")
    assert result == "pong"
    assert await client.call("app.version") == "0.0.0-test"
    await client.close()


@pytest.mark.asyncio
async def test_id_correlation(mock_bridge):
    """Multiple concurrent calls should correlate responses by id."""
    client = WSClient(mock_bridge["host"], mock_bridge["port"])
    results = await asyncio.gather(
        client.call("app.ping"),
        client.call("app.ping"),
        client.call("app.ping"),
    )
    assert all(r == "pong" for r in results)
    await client.close()


@pytest.mark.asyncio
async def test_error_propagation(mock_bridge):
    client = WSClient(mock_bridge["host"], mock_bridge["port"])
    with pytest.raises(TestBridgeError) as exc_info:
        await client.call("unknown.method")
    assert exc_info.value.code == -32601
    await client.close()


@pytest.mark.asyncio
async def test_subscribe_callback(mock_bridge):
    client = WSClient(mock_bridge["host"], mock_bridge["port"])
    received: list[dict] = []

    async def on_event(params):
        received.append(params)

    await client.subscribe("test_topic", on_event)
    # Trigger a publish which causes the mock to emit bus.event
    await client.call("bus.publish", {"topic": "test_topic", "payload": {"x": 1}})
    # Give the reader task time to dispatch
    await asyncio.sleep(0.1)
    assert len(received) == 1
    assert received[0]["topic"] == "test_topic"
    await client.close()


@pytest.mark.asyncio
async def test_graceful_close(mock_bridge):
    client = WSClient(mock_bridge["host"], mock_bridge["port"])
    await client.call("app.ping")
    await client.close()
    assert client._ws is None


@pytest.mark.asyncio
async def test_reconnect(mock_bridge):
    """Client should reconnect after server-side close."""
    client = WSClient(mock_bridge["host"], mock_bridge["port"])
    # First call establishes connection
    await client.call("app.ping")
    # Force-close the underlying WS
    await client._ws.close()
    await asyncio.sleep(0.1)
    # Next call should reconnect
    result = await client.call("app.ping")
    assert result == "pong"
    await client.close()


@pytest.mark.asyncio
async def test_connect_timeout_fails_fast():
    server = await asyncio.start_server(lambda r, w: None, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    server.close()
    await server.wait_closed()

    client = WSClient("127.0.0.1", port, connect_timeout=0.2)
    with pytest.raises(TimeoutError, match="Timed out connecting"):
        await client.call("app.ping")
