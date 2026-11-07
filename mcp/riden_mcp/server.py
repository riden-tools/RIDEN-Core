"""MCP server exposing a running riden-cored to agents over stdio."""
from __future__ import annotations

import argparse
import logging
import math
import sys
from contextlib import aclosing
from typing import Any

import anyio
from mcp.server.mcpserver import MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp.types import ToolAnnotations

from .daemon import Daemon, DaemonError, Profile, profile

log = logging.getLogger("riden-mcp")

MAX_SAMPLE_S = 60.0
STATUS_START, STATUS_COUNT = 0, 20
PRESETS = range(1, 10)

READ_ONLY = ToolAnnotations(read_only_hint=True, open_world_hint=False)
CONTROL = ToolAnnotations(
    read_only_hint=False, destructive_hint=True, idempotent_hint=True, open_world_hint=False
)

INSTRUCTIONS = """\
Controls a RIDEN bench power supply through a local riden-cored daemon.
All values are physical units (V, A, W). Setpoints are not outputs: after a change,
call get_status or sample to see what the load actually draws. Write tools exist only
when the server was started with --enable-writes, and are capped by the user's
--max-voltage/--max-current, which may be lower than the device rating."""


class Limits:
    def __init__(self, max_voltage: float, max_current: float):
        self.max_voltage = max_voltage
        self.max_current = max_current

    def check(self, volts: float | None, amps: float | None, p: Profile) -> None:
        for value in (volts, amps):
            if value is not None and not math.isfinite(value):
                raise ToolError(f"{value} is not a finite number")
        if volts is not None:
            if volts < 0 or volts > self.max_voltage:
                raise ToolError(f"{volts} V is outside the allowed 0–{self.max_voltage} V")
            if p.v_max is not None and volts > p.v_max:
                raise ToolError(f"{volts} V exceeds the {p.model_name} maximum of {p.v_max} V")
        if amps is not None:
            if amps < 0 or amps > self.max_current:
                raise ToolError(f"{amps} A is outside the allowed 0–{self.max_current} A")
            if p.i_max is not None and amps > p.i_max:
                raise ToolError(f"{amps} A exceeds the {p.model_name} maximum of {p.i_max} A")


def _status(p: Profile, r: list[int | None], limits: Limits | None) -> dict[str, Any]:
    temp = None if r[5] is None else (-r[5] if r[4] else r[5])
    status: dict[str, Any] = {
        "model": p.model,
        "model_name": p.model_name,
        "firmware": None if r[3] is None else r[3] / 100,
        "serial": None if r[1] is None or r[2] is None else f"{r[1] * 65536 + r[2]:08d}",
        "output_enabled": r[18] == 1,
        "set_voltage": p.volts(r[8]),
        "set_current": p.amps(r[9]),
        "voltage": p.volts(r[10]),
        "current": p.amps(r[11]),
        "power": p.power(r),
        "mode": "CC" if r[17] == 1 else "CV",
        "protection": {1: "OVP", 2: "OCP"}.get(r[16]),
        "input_voltage": None if r[14] is None else r[14] / 100,
        "temperature_c": temp,
        "device_max_voltage": p.v_max,
        "device_max_current": p.i_max,
        "writes_enabled": limits is not None and p.writable,
    }
    if limits:
        status["allowed_max_voltage"] = limits.max_voltage
        status["allowed_max_current"] = limits.max_current
    return status


def _stats(values: list[float]) -> dict[str, float] | None:
    if not values:
        return None
    return {
        "min": min(values),
        "max": max(values),
        "mean": round(sum(values) / len(values), 4),
    }


def build_server(daemon: Daemon, limits: Limits | None) -> MCPServer:
    server = MCPServer("riden", instructions=INSTRUCTIONS, version="0.1.0")

    async def call(fn, *args):
        try:
            return await fn(*args)
        except DaemonError as e:
            raise ToolError(str(e)) from e
        except TimeoutError as e:
            raise ToolError("riden-cored did not answer in time") from e

    async def status(max_age_ms: int = 500) -> tuple[Profile, list[int | None]]:
        p = await call(profile, daemon)
        r = await call(daemon.read, STATUS_START, STATUS_COUNT, max_age_ms)
        return p, r

    @server.tool(annotations=READ_ONLY)
    async def get_status() -> dict[str, Any]:
        """Model, output state, setpoints, measured V/I/P, CC/CV mode, protection and limits."""
        p, r = await status()
        return _status(p, r, limits)

    @server.tool(annotations=READ_ONLY)
    async def sample(duration_s: float = 5.0, max_points: int = 20) -> dict[str, Any]:
        """Record measured voltage, current and power for duration_s seconds (max 60).

        Returns min/max/mean per channel and up to max_points evenly spaced
        [t_s, V, A, W] points (0 for statistics only)."""
        if not 0 < duration_s <= MAX_SAMPLE_S:
            raise ToolError(f"duration_s must be in (0, {MAX_SAMPLE_S}]")
        if not 0 <= max_points <= 500:
            raise ToolError("max_points must be in [0, 500]")
        p = await call(profile, daemon)
        start = await call(daemon.request, {"cmd": "snapshot"})
        rows: list[tuple[float, float, float, float]] = []
        gaps = 0
        previous = start["sequence"]
        t0 = None
        try:
            with anyio.move_on_after(duration_s):
                async with aclosing(daemon.subscribe(previous)) as stream:
                    async for s in stream:
                        gaps += max(0, s["sequence"] - previous - 1)
                        previous = s["sequence"]
                        r = s["registers"]
                        v, i, w = p.volts(r[10]), p.amps(r[11]), p.power(r)
                        if v is None or i is None or w is None:
                            continue
                        t0 = s["completed_ns"] if t0 is None else t0
                        rows.append((round((s["completed_ns"] - t0) / 1e9, 3), v, i, w))
        except DaemonError as e:
            raise ToolError(str(e)) from e
        result: dict[str, Any] = {
            "samples": len(rows),
            "duration_s": rows[-1][0] if rows else 0.0,
            "skipped_samples": gaps,
            "voltage": _stats([row[1] for row in rows]),
            "current": _stats([row[2] for row in rows]),
            "power": _stats([row[3] for row in rows]),
        }
        if max_points and rows:
            step = max(1, len(rows) / max_points)
            result["points"] = [list(rows[int(k * step)]) for k in range(min(max_points, len(rows)))]
        if not rows:
            result["note"] = "no samples arrived; check that the device is connected"
        return result

    if limits is None:
        return server

    async def writable() -> tuple[Profile, list[int | None]]:
        p, r = await status()
        if not p.writes_enabled:
            raise ToolError("riden-cored was started without --allow-writes")
        if not p.writable:
            raise ToolError(f"model {p.model} ({p.model_name}) is not writable")
        return p, r

    async def write(address: int, raw: int, what: str) -> None:
        log.info("write HR%d=%d (%s)", address, raw, what)
        await call(daemon.write, address, [raw])

    async def readback() -> dict[str, Any]:
        # max_age_ms=0 so the cache from before the write is not returned.
        p, r = await status(0)
        return _status(p, r, limits)

    @server.tool(annotations=CONTROL)
    async def set_voltage(volts: float) -> dict[str, Any]:
        """Set the output voltage setpoint in volts. Returns the status read back afterwards."""
        p, _ = await writable()
        limits.check(volts, None, p)
        await write(8, round(volts * p.v_scale), f"{volts} V")
        return await readback()

    @server.tool(annotations=CONTROL)
    async def set_current(amps: float) -> dict[str, Any]:
        """Set the current limit in amps. Returns the status read back afterwards."""
        p, _ = await writable()
        limits.check(None, amps, p)
        await write(9, round(amps * p.i_scale), f"{amps} A")
        return await readback()

    @server.tool(annotations=CONTROL)
    async def set_output(enabled: bool) -> dict[str, Any]:
        """Turn the output on or off. Turning on is refused if the current setpoints
        exceed the allowed limits."""
        p, r = await writable()
        if enabled:
            limits.check(p.volts(r[8]), p.amps(r[9]), p)
        await write(18, 1 if enabled else 0, "output on" if enabled else "output off")
        return await readback()

    @server.tool(annotations=CONTROL)
    async def recall_preset(slot: int) -> dict[str, Any]:
        """Switch the device to data group M1–M9, including its OVP/OCP."""
        if slot not in PRESETS:
            raise ToolError("slot must be 1–9")
        p, _ = await writable()
        raw_v, raw_i = await call(daemon.read, 80 + slot * 4, 2, 0)
        limits.check(p.volts(raw_v), p.amps(raw_i), p)
        await write(19, slot, f"switch to M{slot}")
        await anyio.sleep(0.6)
        return await readback()

    return server


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(prog="riden-mcp", description=__doc__)
    parser.add_argument("--port", type=int, default=19606, help="riden-cored port (default 19606)")
    parser.add_argument("--timeout", type=float, default=5.0, help="daemon request timeout, s")
    parser.add_argument("--enable-writes", action="store_true", help="register control tools")
    parser.add_argument("--max-voltage", type=float, help="highest voltage an agent may set")
    parser.add_argument("--max-current", type=float, help="highest current an agent may set")
    args = parser.parse_args(argv)
    if args.enable_writes and (args.max_voltage is None or args.max_current is None):
        parser.error("--enable-writes requires --max-voltage and --max-current")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be 1–65535")
    # stdout carries the MCP protocol; logs go to stderr.
    logging.basicConfig(stream=sys.stderr, level=logging.INFO, format="%(name)s: %(message)s")
    limits = Limits(args.max_voltage, args.max_current) if args.enable_writes else None
    build_server(Daemon(args.port, args.timeout), limits).run("stdio")


if __name__ == "__main__":
    main()
