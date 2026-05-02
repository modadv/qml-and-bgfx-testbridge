import pytest
from testbridge_mcp.ws_client import WSClient
import testbridge_mcp.server as srv


@pytest.fixture
async def client(mock_bridge):
    c = WSClient(mock_bridge["host"], mock_bridge["port"])
    yield c
    await c.close()


@pytest.fixture(autouse=True)
def inject_client(client):
    """Inject a test WSClient into the server module."""
    original = srv._client
    srv._client = client
    yield
    srv._client = original


@pytest.mark.asyncio
async def test_app_ping(client):
    result = await srv.app_ping()
    assert result["status"] == "ok"
    assert "version" in result


@pytest.mark.asyncio
async def test_app_describe(client):
    result = await srv.app_describe()
    assert result["applicationName"] == "mock-app"
    assert "qml.meta" in result["methods"]


@pytest.mark.asyncio
async def test_bus_publish(mock_bridge, client):
    await srv.bus_publish("my_topic", {"key": "val"})
    assert any(e["topic"] == "my_topic" for e in mock_bridge["events"])


@pytest.mark.asyncio
async def test_bus_wait_for(client):
    result = await srv.bus_wait_for("some_topic", timeout_ms=1000)
    assert isinstance(result, dict)
    assert result["payload"]["waited"] is True


@pytest.mark.asyncio
async def test_qml_find(client):
    handle = await srv.qml_find("login_username")
    assert handle == 12345


@pytest.mark.asyncio
async def test_qml_get(client):
    result = await srv.qml_get(12345, "text")
    assert result == "mock_value"


@pytest.mark.asyncio
async def test_qml_set(client):
    await srv.qml_set(12345, "text", "hello")


@pytest.mark.asyncio
async def test_qml_invoke(client):
    result = await srv.qml_invoke(12345, "someMethod", [1, 2])
    assert result == "mock_value"


@pytest.mark.asyncio
async def test_qml_click(client):
    await srv.qml_click(12345)


@pytest.mark.asyncio
async def test_qml_type(client):
    await srv.qml_type(12345, "hello world")


@pytest.mark.asyncio
async def test_qml_meta(client):
    result = await srv.qml_meta(12345)
    assert result["class"] == "MockItem"
    assert result["properties"][0]["name"] == "visible"


@pytest.mark.asyncio
async def test_screenshot(client):
    b64 = await srv.screenshot()
    assert isinstance(b64, str)
    assert len(b64) > 0


@pytest.mark.asyncio
async def test_screenshot_with_window(client):
    b64 = await srv.screenshot("mainWindow")
    assert isinstance(b64, str)


@pytest.mark.asyncio
async def test_log_grep(client):
    lines = await srv.log_grep("log line", max_lines=100)
    assert isinstance(lines, list)
    assert len(lines) > 0


@pytest.mark.asyncio
async def test_log_wait(client):
    lines = await srv.log_wait("matched", timeout_ms=2000)
    assert isinstance(lines, list)
    assert "matched line" in lines


@pytest.mark.asyncio
async def test_render_caps(client):
    result = await srv.render_caps()
    assert result["backend"] == "bgfx"


@pytest.mark.asyncio
async def test_render_stats(client):
    result = await srv.render_stats()
    assert result["currentFrame"]["updateMs"] == 1.0


@pytest.mark.asyncio
async def test_render_resources(client):
    result = await srv.render_resources()
    assert result["heightfieldReady"] is True


@pytest.mark.asyncio
async def test_test_artifacts(client):
    result = await srv.test_artifacts()
    assert result["available"] is True
    assert "window.grab" in result["recommendedRpc"]


@pytest.mark.asyncio
async def test_shader_compile(client):
    result = await srv.shader_compile("void main() {}")
    assert result["ok"] is True
    assert result["hash"] == "abc123"


@pytest.mark.asyncio
async def test_shader_apply(client):
    result = await srv.shader_apply("abc123")
    assert result["queued"] is True


@pytest.mark.asyncio
async def test_shader_revert(client):
    result = await srv.shader_revert()
    assert result["queued"] is True


@pytest.mark.asyncio
async def test_shader_list(client):
    result = await srv.shader_list()
    assert "terrain_simple.vertex" in result["supportedSlots"]
    assert "terrain_simple.fragment" in result["supportedSlots"]
    assert "overlay_max_elevation.compute" in result["supportedSlots"]


@pytest.mark.asyncio
async def test_app_quit(client):
    await srv.app_quit()
