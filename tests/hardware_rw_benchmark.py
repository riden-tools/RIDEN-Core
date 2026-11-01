#!/usr/bin/env python3
"""Real-device scheduler benchmark. Writes only the values just read from device."""
import concurrent.futures
import json
import pathlib
import socket
import statistics
import subprocess
import sys
import threading
import time

BINARY, DEVICE, OUTPUT = sys.argv[1:4]
PORT = 19610


def call(request):
    with socket.create_connection(("127.0.0.1", PORT), timeout=5) as sock:
        sock.sendall(json.dumps(request).encode() + b"\n")
        return json.loads(sock.makefile("rb").readline())


def distribution(values):
    ordered = sorted(values)
    return {
        "min_ms": min(ordered),
        "p50_ms": statistics.median(ordered),
        "p95_ms": ordered[int((len(ordered) - 1) * 0.95)],
        "p99_ms": ordered[int((len(ordered) - 1) * 0.99)],
        "max_ms": max(ordered),
        "mean_ms": statistics.mean(ordered),
    }


def measured_write(address, values):
    started = time.perf_counter_ns()
    reply = call({"cmd": "write", "start": address, "values": values})
    elapsed = (time.perf_counter_ns() - started) / 1e6
    if not reply.get("ok"):
        raise RuntimeError(f"write HR{address} rejected: {reply}")
    return elapsed


def verify(expected):
    reply = call({"cmd": "read", "start": 8, "count": 12})
    if not reply.get("ok"):
        raise RuntimeError(f"readback failed: {reply}")
    actual = dict(zip(range(8, 20), reply["values"]))
    for address in (8, 9, 15, 18, 19):
        if actual[address] != expected[address]:
            raise RuntimeError(f"HR{address} changed: {expected[address]} -> {actual[address]}")
    return actual


def sampling(port, count, ready):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as sock:
        reader = sock.makefile("rb")
        sock.sendall(b'{"cmd":"subscribe"}\n')
        ready.set()
        frames = [json.loads(reader.readline()) for _ in range(count)]
    intervals = [(b["completed_ns"] - a["completed_ns"]) / 1e6
                 for a, b in zip(frames, frames[1:])]
    return {
        "frames": len(frames),
        "sequence_gaps": sum(max(0, b["sequence"] - a["sequence"] - 1)
                              for a, b in zip(frames, frames[1:])),
        "intervals": distribution(intervals),
    }


proc = subprocess.Popen(
    [BINARY, "--device", DEVICE, "--port", str(PORT), "--mode", "hot",
     "--allow-writes", "--allow-multiwrite"],
    stderr=subprocess.PIPE,
)
try:
    for _ in range(100):
        try:
            snapshot = call({"cmd": "snapshot"})
            if snapshot["registers"][0] not in (None, 0):
                break
        except OSError:
            pass
        time.sleep(0.05)
    else:
        raise RuntimeError("daemon did not identify the device")
    if snapshot["registers"][0] != 60067:
        raise RuntimeError(f"expected attached RD6006 60067, got HR0={snapshot['registers'][0]}")

    full = call({"cmd": "read", "start": 0, "count": 121})
    if not full.get("ok") or len(full["values"]) != 121:
        raise RuntimeError("could not snapshot all registers before write test")
    registers = full["values"]
    initial = {address: registers[address] for address in (8, 9, 15, 18, 19, 72)}
    if not 0 <= initial[72] <= 5:
        raise RuntimeError(f"HR72 value is outside official range: {initial[72]}")

    phases = {}

    def phase(name, count, action):
        captured = []
        ready = threading.Event()
        subscriber = threading.Thread(target=lambda: captured.append(sampling(PORT, count, ready)))
        started = time.perf_counter()
        subscriber.start()
        if not ready.wait(3):
            raise RuntimeError(f"{name}: subscriber failed to start")
        action()
        subscriber.join(45)
        if subscriber.is_alive():
            raise RuntimeError(f"{name}: subscriber did not finish")
        phases[name] = {**captured[0], "wall_s": time.perf_counter() - started}

    phase("idle", 50, lambda: None)

    single = {address: [] for address in (8, 9, 72)}

    def singles():
        for _ in range(20):
            for address in single:
                single[address].append(measured_write(address, [initial[address]]))

    phase("single_0x06", 130, singles)

    batch = []

    def batches():
        for _ in range(30):
            batch.append(measured_write(8, [initial[8], initial[9]]))

    phase("batch_0x10", 90, batches)

    before_queue = call({"cmd": "snapshot"})
    barrier = threading.Barrier(4)

    def queued_writes(_):
        barrier.wait()
        return [measured_write(8, [initial[8]]) for _ in range(10)]

    queued = []

    def concurrent_writes():
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            queued.extend(pool.map(queued_writes, range(4)))

    concurrent_started = time.perf_counter()
    phase("concurrent_0x06", 80, concurrent_writes)
    concurrent_elapsed = time.perf_counter() - concurrent_started
    final = verify(initial)
    final_full = call({"cmd": "read", "start": 0, "count": 121})["values"]
    if final_full[72] != initial[72]:
        raise RuntimeError(f"HR72 changed: {initial[72]} -> {final_full[72]}")
    after_queue = call({"cmd": "snapshot"})

    result = {
        "model": 60067,
        "device": DEVICE,
        "writes_preserved_values": initial,
        "readback_values": {address: final[address] for address in initial if address in final},
        "single_0x06_same_value": {
            f"HR{address}": {"count": len(times), **distribution(times)}
            for address, times in single.items()
        },
        "multi_0x10_HR8_9_same_values": {"count": len(batch), **distribution(batch)},
        "concurrent_0x06_same_value": {
            "clients": 4,
            "writes": sum(len(client) for client in queued),
            "wall_s": concurrent_elapsed,
            "completed_transactions": after_queue["transactions"] - before_queue["transactions"],
            "failures_delta": after_queue["failures"] - before_queue["failures"],
        },
        "sampling_phases": phases,
        "writes_enabled": True,
        "only_same_existing_values_written": True,
    }
    pathlib.Path(OUTPUT).write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
finally:
    proc.terminate()
    proc.wait(5)
    log = proc.stderr.read().decode()
    if log.strip():
        print(log, file=sys.stderr)
