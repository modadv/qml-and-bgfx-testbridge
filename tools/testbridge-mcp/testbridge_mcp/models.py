from typing import Any
from pydantic import BaseModel


class BusPublishParams(BaseModel):
    topic: str
    payload: Any = None


class BusWaitParams(BaseModel):
    topic: str
    timeout_ms: int = 5000


class QmlFindParams(BaseModel):
    object_name: str


class QmlGetParams(BaseModel):
    handle: int
    property: str


class QmlSetParams(BaseModel):
    handle: int
    property: str
    value: Any


class QmlInvokeParams(BaseModel):
    handle: int
    method: str
    args: list[Any] = []


class QmlClickParams(BaseModel):
    handle: int


class QmlTypeParams(BaseModel):
    handle: int
    text: str


class ScreenshotParams(BaseModel):
    window_name: str | None = None


class LogGrepParams(BaseModel):
    regex: str
    max_lines: int = 200


class LogWaitParams(BaseModel):
    regex: str
    timeout_ms: int = 10000


class AppPingResponse(BaseModel):
    version: str
    status: str = "ok"
