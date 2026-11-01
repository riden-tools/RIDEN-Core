#!/usr/bin/env python3
"""End-to-end test: PTY RD6006 simulator -> riden-cored -> riden-mcp (stdio) -> MCP client.

Usage: python3 mcp/tests/test_server.py PATH/TO/riden-cored
Needs Linux/macOS (PTY) and the `mcp` package.
"""
import os
import pty
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import tty
from pathlib import Path

import anyio
from mcp import Client, StdioServerParameters

CORED = sys.argv[1]
PORT = 19631
LOAD = 10.0
PACKAGE = Path(__file__).resolve().parents[1]


def crc(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


class Device:
    """RD6006 (60067) with a resistive load; writes are recorded."""

    def __init__(self):
        self.master, self.slave = pty.openpty()
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.reg = [0] * 121
        self.reg[0], self.reg[2], self.reg[3] = 60067, 4242, 115
        self.reg[8], self.reg[9], self.reg[14] = 500, 2000, 2400
        for k in range(10):
            self.reg[80 + k * 4 : 84 + k * 4] = [300 + k * 100, 250, 6200, 6200]
        self.reg[80 + 9 * 4] = 4000  # M9 = 40 V, above the test limit
        self.writes = []
        self.running = True
        threading.Thread(target=self.loop, daemon=True).start()

    def update(self):
        r = self.reg
        if not r[18]:
            v = i = 0.0
        else:
            i = min(r[8] / 100 / LOAD, r[9] / 1000)
            v = i * LOAD
        p = round(v * i * 100)
        r[10], r[11], r[12], r[13] = round(v * 100), round(i * 1000), p >> 16, p & 0xFFFF
        r[17] = int(r[18] and r[8] / 100 / LOAD > r[9] / 1000)

    def loop(self):
        buf = b""
        while self.running:
            if not select.select([self.master], [], [], 0.05)[0]:
                continue
            try:
                buf += os.read(self.master, 4096)
            except OSError:
                return
            while len(buf) >= 8:
                fn = buf[1]
                n = 9 + buf[6] if fn == 16 and len(buf) >= 7 else 8
                if len(buf) < n:
                    break
                req, buf = buf[:n], buf[n:]
                addr, fn, start, value = struct.unpack(">BBHH", req[:6])
                self.update()
                time.sleep(0.01)
                if fn == 3:
                    resp = bytes([addr, 3, value * 2]) + struct.pack(
                        f">{value}H", *self.reg[start : start + value]
                    )
                elif fn == 6:
                    self.reg[start] = value
                    self.writes.append((start, value))
                    resp = req[:6]
                else:
                    resp = bytes([addr, fn | 0x80, 1])
                os.write(self.master, resp + struct.pack("<H", crc(resp)))

    def close(self):
        self.running = False
        os.close(self.master)
        os.close(self.slave)


def start_daemon(device, writes):
    args = [CORED, "--device", device.path, "--port", str(PORT), "--mode", "status"]
    if writes:
        args.append("--allow-writes")
    proc = subprocess.Popen(args, stderr=subprocess.DEVNULL)
    end = time.monotonic() + 5
    while time.monotonic() < end:
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            time.sleep(0.5)  # let the first status poll arrive
            return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise AssertionError("riden-cored did not start")


def server(*extra):
    env = {**os.environ, "PYTHONPATH": str(PACKAGE)}
    return StdioServerParameters(
        command=sys.executable,
        args=["-m", "riden_mcp", "--port", str(PORT), *extra],
        env=env,
    )


async def call(client, name, **arguments):
    return await client.call_tool(name, arguments)


async def read_only(device):
    async with Client(server()) as client:
        names = {t.name for t in (await client.list_tools()).tools}
        assert names == {"get_status", "sample"}, names
        s = (await call(client, "get_status")).structured_content
        assert s["model_name"] == "RD6006" and s["set_voltage"] == 5.0, s
        assert s["set_current"] == 2.0 and s["output_enabled"] is False, s
        assert s["device_max_voltage"] == 61.0 and s["writes_enabled"] is False, s
        r = await call(client, "sample", duration_s=0.0)
        assert r.is_error, r
    assert not device.writes, device.writes


async def read_write(device):
    async with Client(server("--enable-writes", "--max-voltage", "12", "--max-current", "1")) as client:
        tools = {t.name: t for t in (await client.list_tools()).tools}
        assert set(tools) >= {"set_voltage", "set_current", "set_output", "recall_preset"}, tools
        assert tools["set_output"].annotations.destructive_hint is True
        assert tools["get_status"].annotations.read_only_hint is True

        # Setpoint 2 A exceeds the 1 A limit, so turning on must be refused.
        r = await call(client, "set_output", enabled=True)
        assert r.is_error and "1.0 A" in r.content[0].text, r
        assert not device.writes, device.writes

        r = await call(client, "set_voltage", volts=13)
        assert r.is_error, r
        r = await call(client, "set_voltage", volts=-1)
        assert r.is_error, r
        r = await call(client, "set_current", amps=float("nan"))
        assert r.is_error, r
        assert not device.writes, device.writes

        s = (await call(client, "set_current", amps=0.4)).structured_content
        assert s["set_current"] == 0.4 and s["allowed_max_current"] == 1.0, s
        s = (await call(client, "set_voltage", volts=3.3)).structured_content
        assert s["set_voltage"] == 3.3, s
        s = (await call(client, "set_output", enabled=True)).structured_content
        assert s["output_enabled"] is True, s

        r = (await call(client, "sample", duration_s=1.5, max_points=5)).structured_content
        assert r["samples"] >= 3 and len(r["points"]) <= 5, r
        assert abs(r["voltage"]["mean"] - 3.3) < 0.02, r
        assert abs(r["current"]["mean"] - 0.33) < 0.002, r
        assert abs(r["power"]["mean"] - 1.089) < 0.02, r

        r = await call(client, "recall_preset", slot=9)  # 40 V
        assert r.is_error, r
        r = await call(client, "recall_preset", slot=0)
        assert r.is_error, r
        s = (await call(client, "recall_preset", slot=2)).structured_content  # 5 V, 0.25 A
        assert s["set_voltage"] == 5.0 and s["set_current"] == 0.25, s

        s = (await call(client, "set_output", enabled=False)).structured_content
        assert s["output_enabled"] is False, s
    assert device.writes == [(9, 400), (8, 330), (18, 1), (8, 500), (9, 250), (18, 0)], device.writes


async def daemon_read_only(device):
    """--enable-writes on the MCP side cannot override a read-only daemon."""
    async with Client(server("--enable-writes", "--max-voltage", "12", "--max-current", "1")) as client:
        r = await call(client, "set_voltage", volts=3)
        assert r.is_error and "--allow-writes" in r.content[0].text, r
    assert not device.writes, device.writes


async def no_daemon():
    async with Client(server()) as client:
        r = await call(client, "get_status")
        assert r.is_error and "cannot connect" in r.content[0].text, r


def run(case, writes):
    device = Device()
    proc = start_daemon(device, writes)
    try:
        anyio.run(case, device)
    finally:
        proc.terminate()
        proc.wait(5)
        device.close()


run(read_only, writes=True)
run(read_write, writes=True)
run(daemon_read_only, writes=False)
anyio.run(no_daemon)
print("MCP: read-only tools, user limits, write readback, sampling, presets and daemon guards passed")
