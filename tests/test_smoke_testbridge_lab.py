import asyncio
import base64
import contextlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get(
    "TESTBRIDGE_LAB_APP",
    ROOT / ".build-release" / "build" / "bin" / "Release" / "testbridge_lab.exe",
))
QT_PREFIX = os.environ.get("QT_PREFIX")
QT_BIN_ENV = os.environ.get("TESTBRIDGE_QT_BIN")
QT_BIN = Path(QT_BIN_ENV) if QT_BIN_ENV else (Path(QT_PREFIX) / "bin" if QT_PREFIX else None)
DEFAULT_ARTIFACT_ROOT = ROOT / "artifacts"


class PngImage:
    def __init__(self, width: int, height: int, rgba: bytes):
        self.width = width
        self.height = height
        self.rgba = rgba

    def pixel(self, x: int, y: int) -> tuple[int, int, int, int]:
        offset = ((y * self.width) + x) * 4
        return tuple(self.rgba[offset:offset + 4])

    def crop(self, box: tuple[int, int, int, int]) -> "PngImage":
        x0, y0, x1, y1 = box
        width = max(0, x1 - x0)
        height = max(0, y1 - y0)
        rgba = bytearray(width * height * 4)
        for y in range(height):
            src = (((y0 + y) * self.width) + x0) * 4
            dst = y * width * 4
            rgba[dst:dst + width * 4] = self.rgba[src:src + width * 4]
        return PngImage(width, height, bytes(rgba))


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _tail(path: Path, limit: int = 4000) -> str:
    if not path.exists():
        return ""
    with path.open("rb") as f:
        f.seek(0, os.SEEK_END)
        size = f.tell()
        f.seek(max(0, size - limit), os.SEEK_SET)
        return f.read().decode("utf-8", errors="replace")


def _wait_for_port(port: int, proc: subprocess.Popen, log_path: Path, timeout_s: float = 20.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"TestBridge app exited before port {port} opened with code {proc.returncode}\n"
                f"app log tail:\n{_tail(log_path)}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError:
            time.sleep(0.25)
    raise TimeoutError(f"TestBridge port {port} did not open\napp log tail:\n{_tail(log_path)}")


def _paeth(left: int, up: int, upper_left: int) -> int:
    p = left + up - upper_left
    pa = abs(p - left)
    pb = abs(p - up)
    pc = abs(p - upper_left)
    if pa <= pb and pa <= pc:
        return left
    if pb <= pc:
        return up
    return upper_left


def _decode_png_rgba(data: bytes) -> PngImage:
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise AssertionError("window.grab did not return a PNG payload")

    offset = 8
    width = height = bit_depth = color_type = None
    idat = bytearray()
    while offset < len(data):
        if offset + 8 > len(data):
            raise AssertionError("truncated PNG chunk header")
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        chunk_type = data[offset + 4:offset + 8]
        chunk_data = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if len(chunk_data) != length:
            raise AssertionError("truncated PNG chunk data")
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, compression, filter_method, interlace = struct.unpack(
                ">IIBBBBB", chunk_data
            )
            if bit_depth != 8:
                raise AssertionError(f"unsupported PNG bit depth: {bit_depth}")
            if color_type not in (0, 2, 4, 6):
                raise AssertionError(f"unsupported PNG color type: {color_type}")
            if compression != 0 or filter_method != 0 or interlace != 0:
                raise AssertionError("unsupported PNG compression, filter, or interlace mode")
        elif chunk_type == b"IDAT":
            idat.extend(chunk_data)
        elif chunk_type == b"IEND":
            break

    if width is None or height is None:
        raise AssertionError("PNG is missing IHDR")

    channels = {0: 1, 2: 3, 4: 2, 6: 4}[color_type]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    expected = (stride + 1) * height
    if len(raw) != expected:
        raise AssertionError(f"unexpected PNG data size: got {len(raw)}, expected {expected}")

    rows: list[bytearray] = []
    pos = 0
    previous = bytearray(stride)
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        scan = bytearray(raw[pos:pos + stride])
        pos += stride
        recon = bytearray(stride)
        for i, value in enumerate(scan):
            left = recon[i - channels] if i >= channels else 0
            up = previous[i]
            upper_left = previous[i - channels] if i >= channels else 0
            if filter_type == 0:
                recon[i] = value
            elif filter_type == 1:
                recon[i] = (value + left) & 0xFF
            elif filter_type == 2:
                recon[i] = (value + up) & 0xFF
            elif filter_type == 3:
                recon[i] = (value + ((left + up) // 2)) & 0xFF
            elif filter_type == 4:
                recon[i] = (value + _paeth(left, up, upper_left)) & 0xFF
            else:
                raise AssertionError(f"unsupported PNG row filter: {filter_type}")
        rows.append(recon)
        previous = recon

    rgba = bytearray(width * height * 4)
    out = 0
    for row in rows:
        for i in range(0, len(row), channels):
            if color_type == 0:
                r = g = b = row[i]
                a = 255
            elif color_type == 2:
                r, g, b = row[i:i + 3]
                a = 255
            elif color_type == 4:
                r = g = b = row[i]
                a = row[i + 1]
            else:
                r, g, b, a = row[i:i + 4]
            rgba[out:out + 4] = bytes((r, g, b, a))
            out += 4
    return PngImage(width, height, bytes(rgba))


def _png_chunk(name: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + name + payload + struct.pack(">I", zlib.crc32(name + payload) & 0xFFFFFFFF)


def _encode_png_rgba(image: PngImage) -> bytes:
    raw = bytearray()
    stride = image.width * 4
    for y in range(image.height):
        raw.append(0)
        raw.extend(image.rgba[y * stride:(y + 1) * stride])
    ihdr = struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", zlib.compress(bytes(raw), level=6))
        + _png_chunk(b"IEND", b"")
    )


def _write_png(path: Path, image: PngImage) -> None:
    path.write_bytes(_encode_png_rgba(image))


def _scaled_region_box(
    image: PngImage,
    scene: dict,
    window_width: float | int | None,
    window_height: float | int | None,
) -> tuple[int, int, int, int]:
    scale_x = image.width / float(window_width) if window_width else 1.0
    scale_y = image.height / float(window_height) if window_height else 1.0
    x0 = max(0, int(float(scene["x"]) * scale_x))
    y0 = max(0, int(float(scene["y"]) * scale_y))
    x1 = min(image.width, int((float(scene["x"]) + float(scene["w"])) * scale_x))
    y1 = min(image.height, int((float(scene["y"]) + float(scene["h"])) * scale_y))

    inset = max(2, int(min(x1 - x0, y1 - y0) * 0.02))
    if x1 - x0 > inset * 2 and y1 - y0 > inset * 2:
        x0 += inset
        y0 += inset
        x1 -= inset
        y1 -= inset
    return x0, y0, x1, y1


def _assert_bgfx_region_has_pixels(
    png: bytes,
    scene: dict,
    window_width: float | int | None,
    window_height: float | int | None,
) -> None:
    image = _decode_png_rgba(png)
    x0, y0, x1, y1 = _scaled_region_box(image, scene, window_width, window_height)
    assert x1 > x0 and y1 > y0, f"invalid bgfx crop box {(x0, y0, x1, y1)} for PNG {image.width}x{image.height}"

    area = (x1 - x0) * (y1 - y0)
    step = max(1, int((area / 50000) ** 0.5))
    colors: Counter[tuple[int, int, int]] = Counter()
    min_luma = 255
    max_luma = 0
    visible = 0

    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            r, g, b, a = image.pixel(x, y)
            if a == 0:
                continue
            visible += 1
            colors[(r, g, b)] += 1
            luma = (r * 299 + g * 587 + b * 114) // 1000
            min_luma = min(min_luma, luma)
            max_luma = max(max_luma, luma)

    assert visible > 1000, f"bgfx crop has too few visible pixels: {visible}"
    dominant = colors.most_common(1)[0][1] / visible if colors else 1.0
    assert len(colors) >= 16, f"bgfx crop looks too close to a solid color: {len(colors)} sampled colors"
    assert dominant < 0.98, f"bgfx crop is dominated by one color: {dominant:.2%}"
    assert max_luma - min_luma >= 12, f"bgfx crop has too little luminance variation: {min_luma}..{max_luma}"


def _compare_images(actual: PngImage, expected: PngImage) -> tuple[dict, PngImage]:
    if actual.width != expected.width or actual.height != expected.height:
        raise AssertionError(
            f"golden size mismatch: actual {actual.width}x{actual.height}, "
            f"expected {expected.width}x{expected.height}"
        )

    total_abs = 0
    max_abs = 0
    changed = 0
    diff = bytearray(actual.width * actual.height * 4)
    pixels = actual.width * actual.height
    for i in range(pixels):
        offset = i * 4
        dr = abs(actual.rgba[offset] - expected.rgba[offset])
        dg = abs(actual.rgba[offset + 1] - expected.rgba[offset + 1])
        db = abs(actual.rgba[offset + 2] - expected.rgba[offset + 2])
        da = abs(actual.rgba[offset + 3] - expected.rgba[offset + 3])
        delta = dr + dg + db + da
        total_abs += delta
        max_abs = max(max_abs, dr, dg, db, da)
        if delta > 24:
            changed += 1
        heat = min(255, delta)
        diff[offset:offset + 4] = bytes((heat, 0, 255 - heat, 255))

    metrics = {
        "width": actual.width,
        "height": actual.height,
        "meanAbsPerChannel": total_abs / float(max(1, pixels * 4)),
        "maxAbsChannel": max_abs,
        "changedPixelRatio": changed / float(max(1, pixels)),
    }
    return metrics, PngImage(actual.width, actual.height, bytes(diff))


def _assert_or_update_bgfx_golden(
    png: bytes,
    scene: dict,
    window_width: float | int | None,
    window_height: float | int | None,
) -> None:
    golden_path_raw = os.environ.get("TESTBRIDGE_GOLDEN_BGFX_REGION")
    if not golden_path_raw:
        return

    image = _decode_png_rgba(png)
    crop = image.crop(_scaled_region_box(image, scene, window_width, window_height))
    golden_path = Path(golden_path_raw)
    update = os.environ.get("TESTBRIDGE_UPDATE_GOLDEN") == "1"

    if update or not golden_path.exists():
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        _write_png(golden_path, crop)
        return

    expected = _decode_png_rgba(golden_path.read_bytes())
    try:
        metrics, diff = _compare_images(crop, expected)
    except AssertionError:
        artifact = _artifact_dir("golden_size_mismatch")
        _write_png(artifact / "actual_crop.png", crop)
        shutil.copyfile(golden_path, artifact / "expected.png")
        raise

    mean_threshold = float(os.environ.get("TESTBRIDGE_IMAGE_MEAN_ABS_THRESHOLD", "6.0"))
    ratio_threshold = float(os.environ.get("TESTBRIDGE_IMAGE_CHANGED_RATIO_THRESHOLD", "0.08"))
    save_artifacts = os.environ.get("TESTBRIDGE_SAVE_ORACLE_ARTIFACTS") == "1"
    failed = (
        metrics["meanAbsPerChannel"] > mean_threshold
        or metrics["changedPixelRatio"] > ratio_threshold
    )
    if failed or save_artifacts:
        artifact = _artifact_dir("golden_diff")
        _write_png(artifact / "actual_crop.png", crop)
        _write_png(artifact / "diff.png", diff)
        shutil.copyfile(golden_path, artifact / "expected.png")
        (artifact / "metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")
        if failed:
            raise AssertionError(
                "bgfx golden image diff exceeded thresholds: "
                f"{metrics}, artifacts: {artifact}"
            )


class Rpc:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0

    async def call(self, method: str, params: dict | None = None, timeout: float = 10.0):
        self.next_id += 1
        msg_id = self.next_id
        await self.ws.send(json.dumps({
            "jsonrpc": "2.0",
            "id": msg_id,
            "method": method,
            "params": params or {},
        }))
        while True:
            raw = await asyncio.wait_for(self.ws.recv(), timeout=timeout)
            resp = json.loads(raw)
            if resp.get("id") != msg_id:
                continue
            if "error" in resp:
                raise RuntimeError(f"{method} failed: {resp['error']}")
            return resp.get("result")


def _json_default(value):
    return str(value)


def _artifact_dir(reason: str) -> Path:
    configured = os.environ.get("TESTBRIDGE_ARTIFACT_DIR")
    root = Path(configured) if configured else DEFAULT_ARTIFACT_ROOT
    stamp = time.strftime("%Y%m%d-%H%M%S")
    safe_reason = "".join(ch if ch.isalnum() or ch in ("-", "_") else "_" for ch in reason[:48])
    path = root / f"testbridge-lab-{stamp}-{safe_reason or 'failure'}"
    path.mkdir(parents=True, exist_ok=True)
    return path


async def _capture_rpc(rpc: Rpc, method: str, params: dict | None = None, timeout: float = 10.0):
    try:
        return {"ok": True, "result": await rpc.call(method, params or {}, timeout=timeout)}
    except Exception as exc:
        return {"ok": False, "error": repr(exc)}


async def collect_artifacts(
    rpc: Rpc,
    log_path: Path,
    reason: str,
    engine_view_handle: int | None = None,
    output_dir: Path | None = None,
) -> Path:
    out = output_dir or _artifact_dir(reason)
    out.mkdir(parents=True, exist_ok=True)

    manifest: dict = {
        "reason": reason,
        "createdAt": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "rpc": {},
        "files": {},
    }

    for method, params in [
        ("app.describe", {}),
        ("test.artifacts", {}),
        ("qml.tree", {"max_depth": 40, "only_visible": False, "max_nodes": 10000}),
        ("render.caps", {}),
        ("render.stats", {}),
        ("render.resources", {}),
        ("log.recent", {"max_lines": 500}),
    ]:
        manifest["rpc"][method] = await _capture_rpc(rpc, method, params)

    if engine_view_handle:
        manifest["rpc"]["qml.geometry.engine_view"] = await _capture_rpc(
            rpc, "qml.geometry", {"handle": engine_view_handle}
        )
        manifest["rpc"]["qml.meta.engine_view"] = await _capture_rpc(
            rpc, "qml.meta", {"handle": engine_view_handle, "include_values": False}
        )

    screenshot = await _capture_rpc(rpc, "window.grab", {}, timeout=15.0)
    manifest["rpc"]["window.grab"] = {"ok": screenshot["ok"]}
    if screenshot["ok"]:
        try:
            png = base64.b64decode(str(screenshot["result"]))
            png_path = out / "screenshot.png"
            png_path.write_bytes(png)
            manifest["files"]["screenshot"] = str(png_path)
        except Exception as exc:
            manifest["rpc"]["window.grab"]["decodeError"] = repr(exc)
    else:
        manifest["rpc"]["window.grab"]["error"] = screenshot["error"]

    if log_path.exists():
        copied = out / "app.log"
        shutil.copyfile(log_path, copied)
        manifest["files"]["appLog"] = str(copied)
        manifest["appLogTail"] = _tail(log_path, limit=12000)

    manifest_path = out / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False, default=_json_default),
        encoding="utf-8",
    )
    return out


async def _grab_until_bgfx_region_renders(
    rpc: Rpc,
    scene: dict,
    window_width: float | int | None,
    window_height: float | int | None,
    timeout_s: float = 8.0,
) -> bytes:
    deadline = time.monotonic() + timeout_s
    last_error: AssertionError | None = None
    while time.monotonic() < deadline:
        png_b64 = await rpc.call("window.grab", {}, timeout=10.0)
        png = base64.b64decode(png_b64)
        try:
            _assert_bgfx_region_has_pixels(png, scene, window_width, window_height)
            return png
        except AssertionError as exc:
            last_error = exc
            await asyncio.sleep(0.5)
    raise AssertionError(f"bgfx region did not render varied pixels before timeout: {last_error}")


LIVE_SHADER_SELFTEST_SOURCE = r'''$input v_texcoord0

#include <bgfx_shader.sh>
#include "uniforms.sh"

SAMPLER2D(u_SmapSampler, 1);
SAMPLER2D(u_DiffuseSampler, 5);

uniform vec4 u_diffuseUvParams;

void main()
{
    vec2 s = texture2D(u_SmapSampler, v_texcoord0).rg * u_DmapFactor;
    vec3 n = normalize(vec3(-s, 1.0));
    float lightFactor = clamp(n.z, 0.1, 1.0);
    vec4 diffuseTexColor = texture2D(u_DiffuseSampler, v_texcoord0);
    vec3 finalColor = mix(diffuseTexColor.rgb, vec3(1.0, 0.12, 0.08), 0.65) * lightFactor;
    gl_FragColor = vec4(finalColor, 1.0);
}
'''


async def _wait_live_shader_state(rpc: Rpc, expected_hash: str | None, timeout_s: float = 8.0) -> dict:
    deadline = time.monotonic() + timeout_s
    last = {}
    while time.monotonic() < deadline:
        with contextlib.suppress(Exception):
            await rpc.call("window.grab", {}, timeout=10.0)
        resources = await rpc.call("render.resources", {}, timeout=10.0)
        live = resources.get("liveShader", {})
        last = live
        if expected_hash is None:
            if not live.get("active"):
                return live
        elif live.get("activeHash") == expected_hash:
            return live
        await asyncio.sleep(0.35)
    raise AssertionError(f"live shader state did not reach expected hash {expected_hash}: {last}")


async def _run_smoke(port: int, log_path: Path) -> None:
    import websockets

    async with websockets.connect(f"ws://127.0.0.1:{port}/", max_size=16 * 1024 * 1024) as ws:
        rpc = Rpc(ws)
        engine_view = None
        try:
            assert await rpc.call("app.ping") == "pong"
            assert await rpc.call("app.version") == "0.1.0-lab"
            describe = await rpc.call("app.describe")
            assert "qml.meta" in describe["methods"]
            assert "render.resources" in describe["methods"]
            assert "shader.compile" in describe["methods"]

            main_window = await rpc.call("qml.find", {"objectName": "main_window"})
            increment = await rpc.call("qml.find", {"objectName": "lab_increment_click"})
            counter_label = await rpc.call("qml.find", {"objectName": "lab_counter_label"})
            engine_view = await rpc.call("qml.find", {"objectName": "lab_engine_view_3d"})

            assert main_window
            assert increment
            assert counter_label
            assert engine_view

            geom = await rpc.call("qml.geometry", {"handle": engine_view})
            assert geom["isQuickItem"] is True
            assert geom["scene"]["w"] > 100
            assert geom["scene"]["h"] > 100
            meta = await rpc.call("qml.meta", {"handle": engine_view, "include_values": False})
            assert meta["objectName"] == "lab_engine_view_3d"
            assert meta["properties"]

            if os.environ.get("TESTBRIDGE_FORCE_ARTIFACT_FAILURE") == "1":
                raise AssertionError("forced artifact failure")

            await rpc.call("qml.click", {"handle": increment})

            text = await rpc.call("qml.get", {"handle": counter_label, "property": "text"})
            assert "Counter: 1" in text

            log_hit = await rpc.call("log.wait", {"regex": "counter incremented to 1", "timeout_ms": 5000}, timeout=8.0)
            assert log_hit and "counter incremented to 1" in log_hit["text"]

            window_width = await rpc.call("qml.get", {"handle": main_window, "property": "width"})
            window_height = await rpc.call("qml.get", {"handle": main_window, "property": "height"})
            png = await _grab_until_bgfx_region_renders(rpc, geom["scene"], window_width, window_height)
            assert png.startswith(b"\x89PNG\r\n\x1a\n")
            _assert_or_update_bgfx_golden(png, geom["scene"], window_width, window_height)

            caps = await rpc.call("render.caps")
            assert caps["available"] is True
            assert caps["backend"] == "bgfx"
            resources = await rpc.call("render.resources")
            assert resources["available"] is True
            assert resources["qmlObjectName"] == "lab_engine_view_3d"
            assert resources["textures"]
            assert resources["drawPrograms"]
            assert resources["computePrograms"]
            assert "buffers" in resources
            stats = await rpc.call("render.stats")
            assert stats["available"] is True
            assert "currentFrame" in stats
            assert "bgfx" in stats
            assert "stats" in stats["bgfx"]
            artifact_caps = await rpc.call("test.artifacts")
            assert artifact_caps["available"] is True

            if os.environ.get("TESTBRIDGE_LIVE_SHADER_SELFTEST") == "1":
                shader_list = await rpc.call("shader.list")
                assert "terrain_simple.fragment" in shader_list["supportedSlots"]
                compiled = await rpc.call(
                    "shader.compile",
                    {"slot": "terrain_simple.fragment", "source": LIVE_SHADER_SELFTEST_SOURCE},
                    timeout=45.0,
                )
                assert compiled["ok"] is True, compiled
                applied = await rpc.call(
                    "shader.apply",
                    {"slot": "terrain_simple.fragment", "hash": compiled["hash"]},
                    timeout=10.0,
                )
                assert applied["queued"] is True
                await _wait_live_shader_state(rpc, compiled["hash"])
                reverted = await rpc.call(
                    "shader.revert",
                    {"slot": "terrain_simple.fragment"},
                    timeout=10.0,
                )
                assert reverted["queued"] is True
                await _wait_live_shader_state(rpc, None)
        except Exception as exc:
            artifact_path = await collect_artifacts(
                rpc,
                log_path=log_path,
                reason=type(exc).__name__,
                engine_view_handle=engine_view,
            )
            raise AssertionError(f"{exc}\nartifacts: {artifact_path}") from exc
        finally:
            with contextlib.suppress(Exception):
                await rpc.call("app.quit", {}, timeout=3.0)


def run_smoke_test(port: int | None = None) -> None:
    if not APP.exists():
        raise FileNotFoundError(f"missing app: {APP}")

    port = port or int(os.environ.get("TESTBRIDGE_PORT") or _free_port())
    env = os.environ.copy()
    env["TESTBRIDGE_PORT"] = str(port)
    if QT_BIN:
        env["PATH"] = str(QT_BIN) + os.pathsep + env.get("PATH", "")

    with tempfile.TemporaryDirectory(prefix="testbridge-lab-smoke-") as tmp:
        log_path = Path(tmp) / "app.log"
        with log_path.open("w", encoding="utf-8", errors="replace") as log:
            proc = subprocess.Popen(
                [str(APP)],
                cwd=str(APP.parent),
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
            )
            try:
                _wait_for_port(port, proc, log_path)
                asyncio.run(_run_smoke(port, log_path))
                with contextlib.suppress(subprocess.TimeoutExpired):
                    proc.wait(timeout=5)
            except Exception as exc:
                log.flush()
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                raise AssertionError(f"{exc}\napp log tail:\n{_tail(log_path)}") from exc
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()


def test_real_gui_smoke() -> None:
    import pytest

    pytest.importorskip("websockets")
    if not APP.exists():
        pytest.skip(f"missing app: {APP}")
    run_smoke_test(port=_free_port())


def main() -> int:
    try:
        run_smoke_test()
    except FileNotFoundError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
