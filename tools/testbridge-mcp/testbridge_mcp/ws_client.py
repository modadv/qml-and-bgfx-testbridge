import asyncio
import json
import logging
import os
import time
from typing import Any, Callable

import websockets
from websockets.exceptions import ConnectionClosed
try:
    from websockets.protocol import State
except Exception:  # pragma: no cover - compatibility with older websockets
    State = None  # type: ignore[assignment]

logger = logging.getLogger(__name__)


class TestBridgeError(Exception):
    def __init__(self, code: int, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


class WSClient:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 47600,
        connect_timeout: float | None = None,
    ) -> None:
        self._uri = f"ws://{host}:{port}/"
        self._connect_timeout = (
            float(os.environ.get("TESTBRIDGE_CONNECT_TIMEOUT", "30"))
            if connect_timeout is None
            else connect_timeout
        )
        self._ws: Any = None
        self._pending: dict[int, asyncio.Future] = {}
        self._subscriptions: dict[str, list[Callable]] = {}
        self._next_id = 1
        self._id_lock = asyncio.Lock()
        self._connect_lock = asyncio.Lock()
        self._reader_task: asyncio.Task | None = None

    def _is_open(self) -> bool:
        if self._ws is None:
            return False
        # websockets >= 13 uses .open; older used .closed
        if hasattr(self._ws, "open"):
            return bool(self._ws.open)
        if hasattr(self._ws, "closed"):
            return not self._ws.closed
        if State is not None and hasattr(self._ws, "state"):
            return self._ws.state == State.OPEN
        return False

    async def _connect(self) -> None:
        delay = 0.5
        deadline = time.monotonic() + max(0.0, self._connect_timeout)
        last_exc: Exception | None = None
        while True:
            try:
                remaining = max(0.1, deadline - time.monotonic())
                self._ws = await websockets.connect(
                    self._uri,
                    max_size=int(os.environ.get("TESTBRIDGE_MAX_SIZE", str(16 * 1024 * 1024))),
                    ping_interval=10,
                    ping_timeout=10,
                    open_timeout=min(10.0, remaining),
                )
                logger.info("Connected to TestBridge at %s", self._uri)
                self._reader_task = asyncio.create_task(self._reader())
                return
            except Exception as exc:
                last_exc = exc
                if time.monotonic() >= deadline:
                    raise TimeoutError(
                        f"Timed out connecting to TestBridge at {self._uri} "
                        f"after {self._connect_timeout:.1f}s"
                    ) from last_exc
                logger.warning("TestBridge connect failed: %s — retrying in %.1fs", exc, delay)
                await asyncio.sleep(min(delay, max(0.0, deadline - time.monotonic())))
                delay = min(delay * 2, 5.0)

    async def _ensure_connected(self) -> None:
        if self._is_open():
            return
        async with self._connect_lock:
            if not self._is_open():
                await self._connect()

    async def _reader(self) -> None:
        try:
            async for raw in self._ws:
                try:
                    msg = json.loads(raw)
                except json.JSONDecodeError:
                    logger.warning("Invalid JSON from TestBridge: %r", raw)
                    continue

                msg_id = msg.get("id")
                if msg_id is not None:
                    fut = self._pending.pop(msg_id, None)
                    if fut and not fut.done():
                        if "error" in msg:
                            err = msg["error"]
                            fut.set_exception(
                                TestBridgeError(err.get("code", -1), err.get("message", "unknown"))
                            )
                        else:
                            fut.set_result(msg.get("result"))
                else:
                    # Notification
                    method = msg.get("method", "")
                    params = msg.get("params", {})
                    if method == "bus.event":
                        topic = params.get("topic", "")
                        for cb in self._subscriptions.get(topic, []):
                            try:
                                asyncio.create_task(cb(params)) if asyncio.iscoroutinefunction(cb) else cb(params)
                            except Exception:
                                logger.exception("Subscription callback error")
        except ConnectionClosed:
            logger.info("TestBridge connection closed")
            self._ws = None
            # Cancel pending futures
            for fut in self._pending.values():
                if not fut.done():
                    fut.cancel()
            self._pending.clear()

    async def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        await self._ensure_connected()
        async with self._id_lock:
            req_id = self._next_id
            self._next_id += 1

        fut: asyncio.Future = asyncio.get_event_loop().create_future()
        self._pending[req_id] = fut

        request = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            request["params"] = params

        await self._ws.send(json.dumps(request))

        try:
            return await asyncio.wait_for(fut, timeout=timeout)
        except asyncio.TimeoutError:
            self._pending.pop(req_id, None)
            raise TimeoutError(f"TestBridge RPC timeout: {method}")

    async def subscribe(self, topic: str, callback: Callable) -> None:
        self._subscriptions.setdefault(topic, []).append(callback)
        await self.call("bus.subscribe", {"topic": topic})

    async def close(self) -> None:
        if self._reader_task:
            self._reader_task.cancel()
            try:
                await self._reader_task
            except asyncio.CancelledError:
                pass
        if self._ws:
            await self._ws.close()
        self._ws = None
