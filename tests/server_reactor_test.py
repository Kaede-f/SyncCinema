#!/usr/bin/env python3
"""TCP integration tests for SyncCinemaServer, using one isolated process per case."""

import argparse
import contextlib
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.request import ProxyHandler, build_opener

MEDIA = "0123456789abcdef"
OTHER_MEDIA = "fedcba9876543210"
METRICS_HTTP = build_opener(ProxyHandler({}))


def wait_until(check, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if result := check():
            return result
        time.sleep(0.02)
    raise AssertionError("condition was not met before timeout")


def unused_loopback_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class RunningServer:
    def __init__(self, binary):
        self.binary = str(Path(binary).resolve())

    def __enter__(self):
        self.files = contextlib.ExitStack()
        directory = Path(self.files.enter_context(tempfile.TemporaryDirectory()))
        self.output = directory / "stdout.log"
        self.telemetry = directory / "events.jsonl"
        try:
            self.metrics_port = unused_loopback_port()
            env = dict(os.environ, SYNCCINEMA_METRICS_PORT=str(self.metrics_port),
                       SYNCCINEMA_TELEMETRY_PATH=str(self.telemetry))
            output_file = self.files.enter_context(self.output.open("wb"))
            self.process = subprocess.Popen([self.binary], env=env, stdout=output_file,
                                            stderr=subprocess.STDOUT)
            self.files.callback(self.kill)
            wait_until(self.ready)
            return self
        except BaseException:
            self.files.close()
            raise

    def ready(self):
        if self.process.poll() is not None:
            raise AssertionError(self.output.read_text(errors="replace"))
        try:
            return self.metric("server_up") == 1
        except OSError:
            return False

    def metric(self, name):
        with METRICS_HTTP.open(f"http://127.0.0.1:{self.metrics_port}/metrics", timeout=2) as response:
            prefix = f"synccinema_{name} "
            for line in response.read().decode().splitlines():
                if line.startswith(prefix):
                    return float(line[len(prefix):])
        raise AssertionError(f"missing metric {name}")

    def events(self):
        contents = self.telemetry.read_text(encoding="utf-8")
        return [json.loads(line) for line in contents.split("\n")[:-1]]

    def stop(self):
        self.process.terminate()
        self.process.wait(timeout=3)
        if self.process.returncode != 0:
            raise AssertionError(f"exit {self.process.returncode}\n{self.output.read_text(errors='replace')[-5000:]}")

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=3)

    def __exit__(self, error_type, error, traceback):
        try:
            if error_type is None and self.process.poll() is None:
                self.stop()
            if error_type is None:
                log = self.output.read_text(errors="replace")
                if any(marker in log for marker in ("AddressSanitizer", "ThreadSanitizer", "runtime error:")):
                    raise AssertionError(log[-6000:])
        finally:
            self.files.close()


class Peer:
    def __init__(self, receive_buffer=None, port=9000):
        self.socket = socket.socket()
        self.socket.settimeout(3)
        try:
            if receive_buffer is not None:
                self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
            self.socket.connect(("127.0.0.1", port))
            self.input = self.socket.makefile("rb")
        except BaseException:
            self.socket.close()
            raise

    def send(self, data):
        self.socket.sendall(data.encode())

    def next_line(self, prefix=None):
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            line = self.input.readline().decode().strip()
            if prefix is None or line.startswith(prefix):
                return line
            if not line:
                raise EOFError(f"waiting for {prefix}")
            if line.startswith("PING "):
                self.send(f"PONG {line.split()[1]}\n")
        raise AssertionError(f"waiting for {prefix}")

    def join(self, media=MEDIA):
        self.send(f"JOIN {media}\n")
        return self.next_line()

    def close(self):
        self.input.close()
        self.socket.close()


class ReactorTests(unittest.TestCase):
    def setUp(self):
        self.server = RunningServer(self.binary).__enter__()
        self.addCleanup(self.server.__exit__, None, None, None)

    def peer(self, receive_buffer=None, port=9000):
        client = Peer(receive_buffer, port)
        self.addCleanup(client.close)
        return client

    def test_handshake_deadline_rejection_and_line_limit(self):
        idle, invalid, mismatch, joined = [self.peer() for _ in range(4)]
        idle.socket.settimeout(7)
        invalid.send("PLAY\n")
        self.assertEqual(invalid.next_line(), "REJECT JOIN_REQUIRED")
        self.assertEqual(invalid.next_line(), "")
        self.assertEqual(joined.join(), f"SNAPSHOT 0 Stopped 0 {MEDIA}")
        self.assertEqual(mismatch.join(OTHER_MEDIA), "REJECT MEDIA_MISMATCH")
        self.assertEqual(mismatch.next_line(), "")
        self.assertEqual(idle.next_line(), "REJECT JOIN_REQUIRED")
        joined.send("x" * 4097)
        while line := joined.next_line():
            self.assertTrue(line.startswith("PING "))
        wait_until(lambda: self.server.metric("connected_clients") == 0)
        next_peer = self.peer()
        self.assertEqual(next_peer.join(OTHER_MEDIA), f"SNAPSHOT 0 Stopped 0 {OTHER_MEDIA}")

    def test_split_join_combined_control_half_close(self):
        peer = self.peer()
        peer.send("\r\nJO")
        time.sleep(0.03)
        peer.send(f"IN {MEDIA}\r\nPLAY\nPAUSE\nSE")
        peer.send("EK 17\n")
        peer.socket.shutdown(socket.SHUT_WR)
        self.assertEqual(peer.next_line(), f"SNAPSHOT 0 Stopped 0 {MEDIA}")
        self.assertEqual([peer.next_line("CONTROL ") for _ in range(3)],
                         ["CONTROL 1 PLAY", "CONTROL 2 PAUSE", "CONTROL 3 SEEK 17000"])
        self.assertEqual(peer.next_line(), "")

    def test_parallel_join_control_echo_ack_and_fd_reuse(self):
        from concurrent.futures import ThreadPoolExecutor

        peers = [self.peer() for _ in range(12)]
        with ThreadPoolExecutor(12) as pool:
            self.assertEqual(list(pool.map(lambda peer: peer.join(), peers)),
                             [f"SNAPSHOT 0 Stopped 0 {MEDIA}"] * len(peers))
            list(pool.map(lambda pair: pair[0].send(f"SEEK {pair[1]}\n"), zip(peers, range(12))))
        reference = [peers[0].next_line("CONTROL ") for _ in peers]
        self.assertEqual([int(line.split()[1]) for line in reference], list(range(1, 13)))
        for peer in peers[1:]:
            self.assertEqual([peer.next_line("CONTROL ") for _ in peers], reference)
        for peer in peers:
            peer.send("".join(f"CONTROL_RESULT {line.split()[1]} SEEK APPLIED "
                              f"{line.split()[3]} 0 OK\n" for line in reference))
        wait_until(lambda: self.server.metric('control_results_total{status="applied"}') == 144)
        for _ in range(30):
            old = self.peer()
            self.assertEqual(old.join(), f"SNAPSHOT 12 Stopped {reference[-1].split()[3]} {MEDIA}")
            old.socket.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            old.close()
        wait_until(lambda: self.server.metric("connected_clients") == len(peers))

    def test_report_rtt_correction_and_convergence(self):
        port = unused_loopback_port()
        proxy_script = Path(__file__).resolve().parents[1] / "tools/observability/tcp_fault_proxy.py"
        proxy = subprocess.Popen(
            [sys.executable, str(proxy_script), "--listen-port", str(port),
             "--upstream-host", "127.0.0.1", "--latency-ms", "25", "--seed", "7"],
            stdout=subprocess.PIPE, text=True,
        )

        def stop_proxy():
            try:
                proxy.terminate()
                proxy.wait(timeout=3)
            finally:
                if proxy.poll() is None:
                    proxy.kill()
                    proxy.wait(timeout=3)
                proxy.stdout.close()

        self.addCleanup(stop_proxy)
        self.assertTrue(select.select([proxy.stdout], [], [], 5)[0], "fault proxy did not start")
        self.assertEqual(json.loads(proxy.stdout.readline())["event"], "proxy_started")
        peers = [self.peer(), self.peer(port=port)]
        for peer in peers:
            self.assertTrue(peer.join().startswith("SNAPSHOT "))
        # Paused positions are constant so persistent skew is deterministic.
        peers[0].send("PAUSE\n")
        for peer in peers:
            self.assertEqual(peer.next_line("CONTROL "), "CONTROL 1 PAUSE")
            ping = peer.next_line("PING ")
            peer.send(f"PONG {ping.split()[1]}\n")
        time.sleep(2.1)

        def send_reports():
            for _ in range(20):
                peers[0].send("REPORT 1 5000 Paused\n")
                peers[1].send("REPORT 1 4000 Paused\n")
                if received.wait(0.15):
                    break

        from concurrent.futures import ThreadPoolExecutor
        from threading import Event

        received = Event()
        with ThreadPoolExecutor(1) as pool:
            reports = pool.submit(send_reports)
            try:
                correct = peers[1].next_line("CORRECT ").split()
            finally:
                received.set()
            reports.result()
        _, command, epoch, state, forward = correct
        self.assertEqual((epoch, state), ("1", "Paused"))
        self.assertGreaterEqual(int(forward), 750)
        peers[1].send(f"CORRECT_RESULT {command} 1 APPLIED 5000 OK\n")
        for _ in range(3):
            for peer in peers:
                peer.send("REPORT 1 5000 Paused\n")
            time.sleep(0.22)
        self.assertEqual(self.server.metric('corrections_total{status="applied"}'), 1)
        self.assertEqual(self.server.metric('corrections_total{status="converged"}'), 1)

    def test_backpressure_isolates_non_reader(self):
        slow = self.peer(1024)
        fast = self.peer()
        self.assertTrue(slow.join().startswith("SNAPSHOT "))
        self.assertTrue(fast.join().startswith("SNAPSHOT "))

        def drive():
            fast.socket.settimeout(10)
            count = 96000
            for start in range(0, count, 400):
                fast.send("SEEK 1200000\n" * 400)
                for epoch in range(start + 1, start + 401):
                    self.assertEqual(fast.next_line("CONTROL "), f"CONTROL {epoch} SEEK 1200000000")
                if self.server.metric("connected_clients") == 1:
                    return
            self.fail("slow connection was not disconnected")

        # Reader and writer share a single socket and are paced in small batches.
        drive()
        wait_until(lambda: self.server.metric("transport_queue_limit_disconnects_total") == 1)
        self.assertGreater(self.server.metric("transport_peak_queued_bytes"), 0)
        self.assertGreater(self.server.metric("control_broadcast_failures_total"), 0)

    def test_sigterm_cleans_up_room_with_idle_scrape(self):
        peer = self.peer()
        self.assertTrue(peer.join().startswith("SNAPSHOT "))
        with socket.create_connection(("127.0.0.1", self.server.metrics_port)) as scrape:
            scrape.sendall(b"GET /metrics HTTP/1.1\r\n")
            self.server.stop()
        self.assertEqual(peer.next_line(), "")
        self.assertTrue(any(e.get("event") == "session_summary" for e in self.server.events()))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args, unittest_args = parser.parse_known_args()
    ReactorTests.binary = args.server
    unittest.main(argv=[__file__, *unittest_args])
