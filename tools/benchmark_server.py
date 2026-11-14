#!/usr/bin/env python3
"""Compare Linux SyncCinemaServer binaries over loopback with the same wire workload."""

import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import platform
import socket
import struct
import subprocess
import time
import tempfile
from urllib.request import ProxyHandler, build_opener

HOST = "127.0.0.1"
MEDIA = "0123456789abcdef"
METRICS_HTTP = build_opener(ProxyHandler({}))


def milliseconds(seconds):
    return seconds * 1000


def percentiles(values):
    if not values:
        return None
    values = sorted(values)
    return {f"p{percent}": round(values[math.ceil(len(values) * percent / 100) - 1], 3)
            for percent in (50, 95, 99)}


def resources(pid):
    proc = Path("/proc") / str(pid)
    status = dict(line.split(":", 1) for line in (proc / "status").read_text().splitlines())
    stat = (proc / "stat").read_text().rsplit(") ", 1)[1].split()
    return {
        "threads": int(status["Threads"]),
        "rss_kib": int(status["VmRSS"].split()[0]),
        "fds": len(list((proc / "fd").iterdir())),
        "cpu_ticks": int(stat[11]) + int(stat[12]),
    }


def get_metrics(port):
    with METRICS_HTTP.open(f"http://{HOST}:{port}/metrics", timeout=2) as response:
        return {key: float(value) for line in response.read().decode().splitlines()
                if line and not line.startswith("#") for key, value in [line.split()]}


async def open_peer(port, slow=False):
    loop = asyncio.get_running_loop()
    sock = socket.socket()
    sock.setblocking(False)
    try:
        if slow:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        await asyncio.wait_for(loop.sock_connect(sock, (HOST, port)), 5)
        join_started = time.perf_counter()
        # Read just the handshake before handing the socket off. The slow-peer
        # scenario leaves no background reader attached after this point.
        await loop.sock_sendall(sock, f"JOIN {MEDIA}\n".encode())
        line = bytearray()
        async def read_snapshot():
            while not line.endswith(b"\n"):
                byte = await loop.sock_recv(sock, 1)
                if not byte or len(line) >= 4096:
                    raise ValueError("connection closed or JOIN response too long")
                line.extend(byte)
        await asyncio.wait_for(read_snapshot(), 5)
        words = line.split()
        if len(words) != 5 or words[0] != b"SNAPSHOT" or words[-1].decode() != MEDIA:
            raise ValueError(f"bad snapshot {line!r}")
        return sock, int(words[1]), milliseconds(time.perf_counter() - join_started)
    except BaseException:
        sock.close()
        raise


class Traffic:
    def __init__(self):
        self.loop = asyncio.get_running_loop()
        self.sent = {}
        self.latency = []
        self.delivered = 0
        self.errors = []

    async def read_peer(self, sock, epoch):
        pending = b""
        try:
            while data := await self.loop.sock_recv(sock, 16384):
                lines = (pending + data).split(b"\n")
                pending = lines.pop()
                replies = []
                for line in lines:
                    words = line.split()
                    if words[0] == b"PING":
                        replies.append(b"PONG " + words[1] + b"\n")
                        continue
                    if words[0] != b"CONTROL" or int(words[1]) != epoch + 1:
                        raise ValueError(f"expected epoch {epoch + 1}, received {line!r}")
                    epoch += 1
                    started, remaining = self.sent[epoch]
                    expected = CONTROL_COMMANDS[(epoch - 1) % len(CONTROL_COMMANDS)].split()
                    position = words[3] if len(words) > 3 else b"0"
                    if words[2] != expected[0] or (words[2] == b"SEEK" and position != b"1200000000"):
                        raise ValueError(f"incorrect control payload {line!r}")
                    self.latency.append(milliseconds(time.perf_counter() - started))
                    self.delivered += 1
                    if remaining > 1:
                        self.sent[epoch] = (started, remaining - 1)
                    else:
                        del self.sent[epoch]
                    replies.append(b"CONTROL_RESULT " + words[1] + b" " + words[2]
                                   + b" APPLIED " + position + b" 0 OK\n")
                if replies:
                    await self.loop.sock_sendall(sock, b"".join(replies))
            self.errors.append("reader closed before the scenario ended")
        except (OSError, ValueError, KeyError, IndexError) as error:
            self.errors.append(str(error))

    async def drive(self, sock, epoch, readers, count, window, batch_timeout):
        start = time.perf_counter()
        sent = 0
        while sent < count:
            batch = min(window, count - sent)
            started = time.perf_counter()
            wire = []
            for _ in range(batch):
                wire.append(CONTROL_COMMANDS[epoch % len(CONTROL_COMMANDS)])
                epoch += 1
                self.sent[epoch] = (started, readers)
            sent += batch
            await asyncio.wait_for(self.loop.sock_sendall(sock, b"".join(wire)), batch_timeout)
            deadline = started + batch_timeout
            while self.sent:
                if self.errors:
                    raise RuntimeError(self.errors[0])
                if time.perf_counter() > deadline:
                    raise TimeoutError(f"epoch {min(self.sent)} stalled after {self.delivered} echoes")
                await asyncio.sleep(0.0005)
        return time.perf_counter() - start


CONTROL_COMMANDS = [b"SEEK 1200000\n", b"PLAY\n", b"PAUSE\n"]


async def run_scenario(binary, args, metrics_port, size, slow=False):
    port = 9000
    env = dict(os.environ, SYNCCINEMA_METRICS_PORT=str(metrics_port))
    env.pop("SYNCCINEMA_TELEMETRY_PATH", None)
    # Use memory-backed storage for both binaries and retain startup diagnostics.
    log = tempfile.TemporaryFile(dir="/dev/shm")
    server = subprocess.Popen([binary], env=env, stdout=log, stderr=subprocess.STDOUT)
    sockets, readers, joins, failures = [], [], [], []
    traffic = Traffic()
    initial_epoch = 0
    result = {"target": size, "slow_peer": slow}
    try:
        for _ in range(250):
            if server.poll() is not None:
                log.seek(0)
                raise RuntimeError(f"server startup failed: {log.read(2048).decode(errors='replace')}")
            try:
                if get_metrics(metrics_port).get("synccinema_server_up") == 1:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.02)
        else:
            raise TimeoutError("metrics endpoint did not become ready")

        idle = resources(server.pid)
        if slow:
            slow_socket, initial_epoch, join_ms = await open_peer(port, slow=True)
            sockets.append(slow_socket)
            joins.append(join_ms)
        # Use the same connection ramp for each architecture.
        for offset in range(len(sockets), size, 32):
            connected = await asyncio.gather(*[open_peer(port) for _ in range(min(32, size - offset))],
                                             return_exceptions=True)
            for item in connected:
                if isinstance(item, BaseException):
                    failures.append(str(item))
                else:
                    sock, epoch, join_ms = item
                    sockets.append(sock)
                    joins.append(join_ms)
                    readers.append(asyncio.create_task(traffic.read_peer(sock, epoch)))
        joined = resources(server.pid)
        result.update(joined=len(sockets), join_ms=percentiles(joins), join_errors=failures,
                      at_join=joined, idle=idle)
        await asyncio.sleep(args.idle_seconds)
        held = resources(server.pid)
        result["after_idle_hold"] = held
        if not readers:
            raise RuntimeError("no active reader joined")

        before = resources(server.pid)
        started = time.perf_counter()
        count = args.slow_commands if slow else args.commands
        window = 400 if slow else args.window
        try:
            duration = await traffic.drive(sockets[-1], initial_epoch, len(readers),
                                           count, window, args.batch_timeout)
        except (TimeoutError, RuntimeError, OSError) as error:
            result["traffic_error"] = str(error)
            duration = time.perf_counter() - started
        after = resources(server.pid)
        result.update(control_ms=percentiles(traffic.latency), delivered=traffic.delivered,
                      echoes_per_second=round(traffic.delivered / duration),
                      control_duration_seconds=round(duration, 3),
                      mean_cpu_cores=round((after["cpu_ticks"] - before["cpu_ticks"]) /
                                           os.sysconf("SC_CLK_TCK") / duration, 4))
        await asyncio.sleep(1.05)
        metrics = get_metrics(metrics_port)
        result["metrics"] = {key.removeprefix("synccinema_"): value for key, value in metrics.items()
                             if key.startswith("synccinema_transport_")
                             or key in {"synccinema_control_expected_acks_total",
                                        "synccinema_control_results_total{status=\"applied\"}",
                                        "synccinema_control_broadcast_failures_total",
                                        "synccinema_connected_clients"}}
        result["reader_errors"] = traffic.errors
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        result["scenario_error"] = str(error)
    finally:
        for task in readers:
            task.cancel()
        await asyncio.gather(*readers, return_exceptions=True)
        for sock in sockets:
            # End disposable benchmark connections without leaving TIME_WAIT on
            # the fixed server port, which the baseline binds without SO_REUSEADDR.
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            sock.close()
        if server.poll() is None:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                current = resources(server.pid)
                idle = result.get("idle", current)
                if current["fds"] == idle["fds"] and current["threads"] == idle["threads"]:
                    result["after_disconnect"] = current
                    break
                await asyncio.sleep(0.02)
            else:
                result["cleanup_timeout"] = True
            if server.poll() is None:
                server.terminate()
            try:
                server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
        result["exit_code"] = server.returncode
        log.close()
    return result


async def main(args):
    rows = []
    for label, binary in (("baseline", args.baseline), ("reactor", args.reactor)):
        cases = [(size, False) for size in args.connections]
        if args.slow_commands:
            cases.append((2, True))
        for size, slow in cases:
            row = await run_scenario(str(Path(binary).resolve()), args, args.metrics_port, size, slow)
            rows.append(dict(architecture=label, **row))
            print(json.dumps(rows[-1]), flush=True)
    report = {"kernel": platform.release(), "cpu_count": os.cpu_count(),
              "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
              "parameters": vars(args), "measurements": rows}
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    return any(
        row.get("scenario_error")
        or row.get("cleanup_timeout")
        or row.get("reader_errors")
        or row.get("joined") != row["target"]
        or (not row["slow_peer"] and row.get("delivered") != row["target"] * args.commands)
        or (row["slow_peer"] and row["architecture"] == "reactor"
            and row.get("delivered") != args.slow_commands)
        for row in rows
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--reactor", required=True)
    parser.add_argument("--connections", type=int, nargs="+", default=[10, 100, 500, 1000])
    parser.add_argument("--commands", type=int, default=160)
    parser.add_argument("--window", type=int, default=4)
    parser.add_argument("--idle-seconds", type=float, default=1.0)
    parser.add_argument("--batch-timeout", type=float, default=3.0)
    parser.add_argument("--slow-commands", type=int, default=0)
    parser.add_argument("--metrics-port", type=int, default=19100)
    parser.add_argument("--output", required=True)
    parsed = parser.parse_args()
    if min(*parsed.connections, parsed.commands, parsed.window) < 1 or parsed.slow_commands < 0:
        parser.error("connections, commands and window must be positive, slow-commands nonnegative")
    if parsed.connections and max(parsed.connections) > 1000:
        parser.error("the baseline uses FD_SET, so this comparison caps the load at 1000 clients")
    raise SystemExit(asyncio.run(main(parsed)))
