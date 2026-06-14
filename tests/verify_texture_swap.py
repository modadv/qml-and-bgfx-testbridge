"""
Standalone verification script: runtime texture swapping in testbridge_lab.

Exercises QUrl property swapping (heightfieldSource / diffuseSource) on the
lab_engine_view_3d object through the TestBridge WebSocket RPC, checking:

  (a) The app never crashes / port stays alive.
  (b) render.resources texture handle counts stay BOUNDED (no handle leak).
  (c) The bgfx region keeps rendering varied (non-solid) pixels after each swap.

Run as:
    python tests/verify_texture_swap.py

Exit 0 on success, non-zero on failure.
"""

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


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

ROOT = Path(__file__).resolve().parents[1]
APP = Path(os.environ.get(
    "TESTBRIDGE_LAB_APP",
    ROOT / ".build-release" / "build" / "bin" / "Release" / "testbridge_lab.exe",
))
ASSETS_DIR = ROOT / ".build-release" / "build" / "bin" / "Release" / "assets"

# Qt environment — required for the app to find its platform plugin.
_QT_BIN = Path(r"E:\DevEnv\conan_home\p\b\qtd78c6f08175fb\p\bin")
_QT_PLUGINS = _QT_BIN / "archdatadir" / "plugins"


def _build_env() -> dict:
    env = os.environ.copy()
    env["PATH"] = str(_QT_BIN) + os.pathsep + env.get("PATH", "")
    env["QT_PLUGIN_PATH"] = str(_QT_PLUGINS)
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(_QT_PLUGINS / "platforms")
    return env


# ---------------------------------------------------------------------------
# Minimal PNG decoder (copied from test_smoke_testbridge_lab.py)
# ---------------------------------------------------------------------------

class PngImage:
    def __init__(self, width: int, height: int, rgba: bytes):
        self.width = width
        self.height = height
        self.rgba = rgba

    def pixel(self, x: int, y: int) -> tuple:
        offset = ((y * self.width) + x) * 4
        return tuple(self.rgba[offset:offset + 4])


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

    rows = []
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
                r = g = b = row[i]; a = 255
            elif color_type == 2:
                r, g, b = row[i:i + 3]; a = 255
            elif color_type == 4:
                r = g = b = row[i]; a = row[i + 1]
            else:
                r, g, b, a = row[i:i + 4]
            rgba[out:out + 4] = bytes((r, g, b, a))
            out += 4
    return PngImage(width, height, bytes(rgba))


# ---------------------------------------------------------------------------
# Pixel-variety assertions (reused from smoke test)
# ---------------------------------------------------------------------------

def _assert_bgfx_region_has_pixels(
    png: bytes,
    scene: dict,
    window_width,
    window_height,
) -> None:
    image = _decode_png_rgba(png)

    scale_x = image.width / float(window_width) if window_width else 1.0
    scale_y = image.height / float(window_height) if window_height else 1.0
    x0 = max(0, int(float(scene["x"]) * scale_x))
    y0 = max(0, int(float(scene["y"]) * scale_y))
    x1 = min(image.width, int((float(scene["x"]) + float(scene["w"])) * scale_x))
    y1 = min(image.height, int((float(scene["y"]) + float(scene["h"])) * scale_y))

    inset = max(2, int(min(x1 - x0, y1 - y0) * 0.02))
    if x1 - x0 > inset * 2 and y1 - y0 > inset * 2:
        x0 += inset; y0 += inset; x1 -= inset; y1 -= inset

    assert x1 > x0 and y1 > y0, f"invalid bgfx crop box {(x0,y0,x1,y1)} for PNG {image.width}x{image.height}"

    area = (x1 - x0) * (y1 - y0)
    step = max(1, int((area / 50000) ** 0.5))
    colors: Counter = Counter()
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
    assert len(colors) >= 16, f"bgfx crop looks too close to a solid color: {len(colors)} unique colors"
    assert dominant < 0.98, f"bgfx crop is dominated by one color: {dominant:.2%}"
    assert max_luma - min_luma >= 12, f"bgfx crop has too little luminance variation: {min_luma}..{max_luma}"


# ---------------------------------------------------------------------------
# Port helpers
# ---------------------------------------------------------------------------

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


def _wait_for_port(port: int, proc: subprocess.Popen, log_path: Path, timeout_s: float = 30.0) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(
                f"TestBridge app exited before port {port} opened (code {proc.returncode})\n"
                f"app log tail:\n{_tail(log_path)}"
            )
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1.0):
                return
        except OSError:
            time.sleep(0.25)
    raise TimeoutError(f"TestBridge port {port} did not open\napp log:\n{_tail(log_path)}")


# ---------------------------------------------------------------------------
# RPC (copied from smoke test)
# ---------------------------------------------------------------------------

class Rpc:
    def __init__(self, ws):
        self.ws = ws
        self.next_id = 0

    async def call(self, method: str, params: dict = None, timeout: float = 10.0):
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


# ---------------------------------------------------------------------------
# Grab helpers
# ---------------------------------------------------------------------------

async def _grab_until_bgfx_region_renders(
    rpc: Rpc,
    scene: dict,
    window_width,
    window_height,
    timeout_s: float = 12.0,
) -> bytes:
    deadline = time.monotonic() + timeout_s
    last_error = None
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


# ---------------------------------------------------------------------------
# Texture-handle leak detection helpers
# ---------------------------------------------------------------------------

def _texture_handle_set(resources: dict) -> set:
    """Return the set of valid (non-BGFX_INVALID_HANDLE) texture handles."""
    result = set()
    for tex in resources.get("textures", []):
        if tex.get("valid"):
            result.add((tex["name"], tex["handle"]))
    return result


def _texture_count(resources: dict) -> int:
    """Count valid textures reported by render.resources."""
    return sum(1 for t in resources.get("textures", []) if t.get("valid"))


# ---------------------------------------------------------------------------
# Core swap test
# ---------------------------------------------------------------------------

async def _run_swap_test(port: int, log_path: Path, swap_assets: list) -> None:
    """
    swap_assets: list of (heightfield_url, diffuse_url) pairs to cycle through.
    """
    import websockets

    async with websockets.connect(f"ws://127.0.0.1:{port}/", max_size=16 * 1024 * 1024) as ws:
        rpc = Rpc(ws)
        engine_view = None
        try:
            # Basic sanity
            assert await rpc.call("app.ping") == "pong"

            # Find the engine view
            engine_view = await rpc.call("qml.find", {"objectName": "lab_engine_view_3d"})
            assert engine_view, "lab_engine_view_3d not found in QML tree"

            main_window = await rpc.call("qml.find", {"objectName": "main_window"})
            window_width = await rpc.call("qml.get", {"handle": main_window, "property": "width"})
            window_height = await rpc.call("qml.get", {"handle": main_window, "property": "height"})

            geom = await rpc.call("qml.geometry", {"handle": engine_view})
            scene = geom["scene"]
            assert scene["w"] > 100 and scene["h"] > 100, f"engine view too small: {scene}"

            # Wait for initial render
            print(f"  [init] waiting for initial bgfx render...", flush=True)
            await _grab_until_bgfx_region_renders(rpc, scene, window_width, window_height)
            print(f"  [init] initial render OK", flush=True)

            # Baseline texture count
            res0 = await rpc.call("render.resources")
            baseline_count = _texture_count(res0)
            print(f"  [init] baseline texture count = {baseline_count}", flush=True)

            # Allow up to 2x baseline as a generous bound (catches unbounded growth).
            # If baseline is 0, allow up to 4 (should never stay at 0 after initial render).
            max_allowed_count = max(baseline_count * 2, baseline_count + 4)

            # --- Swap loop ---
            swap_count = 0
            for i, (hf_url, diff_url) in enumerate(swap_assets):
                print(f"  [swap {i+1}/{len(swap_assets)}] heightfield={hf_url}", flush=True)
                print(f"  [swap {i+1}/{len(swap_assets)}]   diffuse={diff_url}", flush=True)

                # Set heightfieldSource — Qt setProperty can convert QString -> QUrl
                ok_hf = await rpc.call("qml.set", {
                    "handle": engine_view,
                    "property": "heightfieldSource",
                    "value": hf_url,
                })
                ok_diff = await rpc.call("qml.set", {
                    "handle": engine_view,
                    "property": "diffuseSource",
                    "value": diff_url,
                })
                print(f"  [swap {i+1}] qml.set heightfieldSource={ok_hf}, diffuseSource={ok_diff}", flush=True)

                # Give the render thread a moment to process the swap
                await asyncio.sleep(0.3)

                # (a) Assert port still alive — if app crashed the next RPC will fail
                # (b) Assert pixel variety
                png = await _grab_until_bgfx_region_renders(
                    rpc, scene, window_width, window_height, timeout_s=15.0
                )
                print(f"  [swap {i+1}] bgfx render OK", flush=True)

                # (c) Texture handle count bounded
                res = await rpc.call("render.resources")
                current_count = _texture_count(res)
                print(f"  [swap {i+1}] texture count = {current_count} (max allowed = {max_allowed_count})", flush=True)
                assert current_count <= max_allowed_count, (
                    f"TEXTURE LEAK DETECTED after swap {i+1}: "
                    f"count={current_count} exceeds max_allowed={max_allowed_count} "
                    f"(baseline={baseline_count}). "
                    f"textures={res.get('textures')}"
                )

                swap_count += 1

            assert swap_count >= 6, f"Expected >= 6 swaps, only completed {swap_count}"
            print(f"  [done] {swap_count} swaps completed — all assertions passed", flush=True)

        except Exception as exc:
            print(f"\nFAILURE during swap test: {exc}", file=sys.stderr, flush=True)
            # Try to grab a screenshot for debug
            with contextlib.suppress(Exception):
                png_b64 = await rpc.call("window.grab", {}, timeout=10.0)
                png = base64.b64decode(png_b64)
                dbg = Path(tempfile.gettempdir()) / "verify_texture_swap_fail.png"
                dbg.write_bytes(png)
                print(f"  debug screenshot: {dbg}", file=sys.stderr)
            raise
        finally:
            with contextlib.suppress(Exception):
                await rpc.call("app.quit", {}, timeout=3.0)


# ---------------------------------------------------------------------------
# Asset preparation
# ---------------------------------------------------------------------------

def _prepare_swap_assets(tmp_dir: Path) -> list:
    """
    Return a list of >= 6 (heightfield_url, diffuse_url) pairs.

    We only have two source PNGs, so we create copies with distinct paths so
    that the engine's path-change guard (`m_lastHeightfieldPath`) actually
    triggers a reload on each swap.
    """
    hf_src = ASSETS_DIR / "sample_heightfield.png"
    diff_src = ASSETS_DIR / "sample_diffuse.png"

    if not hf_src.exists():
        raise FileNotFoundError(f"Heightfield asset not found: {hf_src}")
    if not diff_src.exists():
        raise FileNotFoundError(f"Diffuse asset not found: {diff_src}")

    # Create several copies in tmp_dir
    copies = []
    for idx in range(4):
        hf_copy = tmp_dir / f"heightfield_{idx}.png"
        diff_copy = tmp_dir / f"diffuse_{idx}.png"
        shutil.copy2(hf_src, hf_copy)
        shutil.copy2(diff_src, diff_copy)
        copies.append((hf_copy, diff_copy))

    # Also include the originals
    copies.append((hf_src, diff_src))

    # Build 8 swap pairs cycling through different path combos
    pairs = []
    n = len(copies)
    for i in range(8):
        hf = copies[i % n][0]
        diff = copies[(i + 1) % n][1]
        pairs.append((
            hf.as_uri(),
            diff.as_uri(),
        ))

    return pairs


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def run_texture_swap_test() -> None:
    if not APP.exists():
        raise FileNotFoundError(f"App binary not found: {APP}")
    if not ASSETS_DIR.exists():
        raise FileNotFoundError(f"Assets directory not found: {ASSETS_DIR}")

    port = int(os.environ.get("TESTBRIDGE_PORT") or _free_port())
    env = _build_env()
    env["TESTBRIDGE_PORT"] = str(port)

    with tempfile.TemporaryDirectory(prefix="testbridge-texswap-") as tmp:
        tmp_path = Path(tmp)
        log_path = tmp_path / "app.log"
        swap_assets = _prepare_swap_assets(tmp_path)

        print(f"[verify_texture_swap] launching app on port {port}", flush=True)
        print(f"[verify_texture_swap] app: {APP}", flush=True)
        print(f"[verify_texture_swap] prepared {len(swap_assets)} swap pairs", flush=True)

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
                print(f"[verify_texture_swap] port open, running swap test...", flush=True)
                asyncio.run(_run_swap_test(port, log_path, swap_assets))
                with contextlib.suppress(subprocess.TimeoutExpired):
                    proc.wait(timeout=5)
                print("[verify_texture_swap] PASSED", flush=True)
            except Exception as exc:
                log.flush()
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()
                app_log_tail = _tail(log_path)
                raise AssertionError(
                    f"Texture swap test FAILED: {exc}\n\nApp log tail:\n{app_log_tail}"
                ) from exc
            finally:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        proc.kill()
                        proc.wait()


def main() -> int:
    try:
        run_texture_swap_test()
    except FileNotFoundError as exc:
        print(f"SETUP ERROR: {exc}", file=sys.stderr)
        return 2
    except AssertionError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"UNEXPECTED ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
