"""Client for the riden-cored NDJSON interface (docs/API.md)."""
from __future__ import annotations

import json
import math
from collections.abc import AsyncIterator
from typing import Any

import anyio
from anyio.streams.buffered import BufferedByteReceiveStream

HOST = "127.0.0.1"
MAX_LINE = 65536


class DaemonError(Exception):
    def __init__(self, message: str, code: int | None = None):
        super().__init__(message)
        self.code = code


class Daemon:
    def __init__(self, port: int, timeout: float = 5.0):
        self.port = port
        self.timeout = timeout

    async def _connect(self):
        try:
            return await anyio.connect_tcp(HOST, self.port)
        except OSError as e:
            raise DaemonError(f"cannot connect to riden-cored on {HOST}:{self.port} ({e})") from e

    @staticmethod
    def _decode(line: bytes) -> dict[str, Any]:
        message = json.loads(line)
        if not message.get("ok"):
            raise DaemonError(message.get("message", "request failed"), message.get("error"))
        return message

    async def request(self, command: dict[str, Any]) -> dict[str, Any]:
        """One request per connection; the daemon allows 32 concurrent connections."""
        with anyio.fail_after(self.timeout):
            stream = await self._connect()
            async with stream:
                await stream.send(json.dumps(command).encode() + b"\n")
                try:
                    line = await BufferedByteReceiveStream(stream).receive_until(b"\n", MAX_LINE)
                except anyio.IncompleteRead as e:
                    raise DaemonError("riden-cored closed the connection") from e
        return self._decode(line)

    async def subscribe(self, after: int) -> AsyncIterator[dict[str, Any]]:
        """Yields snapshots newer than `after` until the caller stops iterating."""
        stream = await self._connect()
        async with stream:
            await stream.send(json.dumps({"cmd": "subscribe", "after": after}).encode() + b"\n")
            reader = BufferedByteReceiveStream(stream)
            while True:
                try:
                    line = await reader.receive_until(b"\n", MAX_LINE)
                except anyio.IncompleteRead as e:
                    raise DaemonError("riden-cored closed the subscription") from e
                yield self._decode(line)

    async def read(self, start: int, count: int, max_age_ms: int = 0) -> list[int]:
        message = await self.request(
            {"cmd": "read", "start": start, "count": count, "max_age_ms": max_age_ms}
        )
        return message["values"]

    async def write(self, start: int, values: list[int]) -> None:
        await self.request({"cmd": "write", "start": start, "values": values})


def decimals(scale: int) -> int:
    return round(math.log10(scale)) if scale > 0 else 0


class Profile:
    """Scaling and limits from the `registers` capability table."""

    def __init__(self, caps: dict[str, Any]):
        regs = caps["registers"]
        self.model = caps["model"]
        self.model_name = caps["model_name"]
        self.known = self.model_name != "Unknown"
        self.writes_enabled = bool(caps.get("writes_enabled"))
        self.writable = self.writes_enabled and regs[8]["writable"] and regs[9]["writable"]
        # HR10/11 share the scale of HR8/9; the table reports profile scales only for HR8/9.
        self.v_scale = regs[8]["scale"] or 100
        self.i_scale = regs[9]["scale"] or 1000
        self.v_max = regs[8]["raw_max"] / self.v_scale if self.known else None
        self.i_max = regs[9]["raw_max"] / self.i_scale if self.known else None

    def volts(self, raw: int | None) -> float | None:
        return None if raw is None else round(raw / self.v_scale, decimals(self.v_scale))

    def amps(self, raw: int | None) -> float | None:
        return None if raw is None else round(raw / self.i_scale, decimals(self.i_scale))

    def power(self, regs: list[int | None]) -> float | None:
        """HR12/13 is a 32-bit 0.01 W value; falls back to V×I when it disagrees by >10%."""
        v, i = self.volts(regs[10]), self.amps(regs[11])
        if v is None or i is None:
            return None
        product = v * i
        if regs[12] is not None and regs[13] is not None:
            reported = (regs[12] * 65536 + regs[13]) / 100
            if abs(reported - product) <= max(0.05, product * 0.1):
                return round(reported, 2)
        return round(product, 3)


async def profile(daemon: Daemon) -> Profile:
    return Profile(await daemon.request({"cmd": "registers"}))
