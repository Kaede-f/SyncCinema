from __future__ import annotations

import runpy
import socket
import threading
import time
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "tcp_fault_proxy.py"
PROXY_MODULE = runpy.run_path(str(MODULE_PATH))
FaultProfile = PROXY_MODULE["FaultProfile"]
FaultProxy = PROXY_MODULE["FaultProxy"]


def unused_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


class TcpFaultProxyTests(unittest.TestCase):
    def test_proxy_forwards_data_and_injects_bidirectional_latency(self) -> None:
        upstream_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        upstream_listener.bind(("127.0.0.1", 0))
        upstream_listener.listen()
        upstream_port = int(upstream_listener.getsockname()[1])

        def echo_once() -> None:
            connection, _address = upstream_listener.accept()
            with connection:
                payload = connection.recv(1024)
                connection.sendall(payload)

        echo_thread = threading.Thread(target=echo_once, daemon=True)
        echo_thread.start()

        proxy_port = unused_local_port()
        proxy = FaultProxy(
            "127.0.0.1",
            proxy_port,
            "127.0.0.1",
            upstream_port,
            FaultProfile(30.0, 0.0, None, 0.0, None),
            7,
        )
        proxy_thread = threading.Thread(target=proxy.run, daemon=True)
        proxy_thread.start()

        client: socket.socket | None = None
        for _attempt in range(20):
            try:
                client = socket.create_connection(("127.0.0.1", proxy_port), timeout=1.0)
                break
            except ConnectionRefusedError:
                time.sleep(0.05)

        self.assertIsNotNone(client)
        assert client is not None
        with client:
            started_at = time.monotonic()
            client.sendall(b"PING\n")
            self.assertEqual(client.recv(1024), b"PING\n")
            elapsed_ms = (time.monotonic() - started_at) * 1000.0

        proxy.stop()
        proxy_thread.join(timeout=2.0)
        upstream_listener.close()

        self.assertGreaterEqual(elapsed_ms, 50.0)
        self.assertFalse(proxy_thread.is_alive())

    def test_scheduled_disconnect_closes_an_idle_connection(self) -> None:
        upstream_listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        upstream_listener.bind(("127.0.0.1", 0))
        upstream_listener.listen()
        upstream_port = int(upstream_listener.getsockname()[1])

        def hold_connection() -> None:
            connection, _address = upstream_listener.accept()
            with connection:
                time.sleep(0.5)

        threading.Thread(target=hold_connection, daemon=True).start()

        proxy_port = unused_local_port()
        proxy = FaultProxy(
            "127.0.0.1",
            proxy_port,
            "127.0.0.1",
            upstream_port,
            FaultProfile(0.0, 0.0, None, 0.0, 0.08),
            9,
        )
        proxy_thread = threading.Thread(target=proxy.run, daemon=True)
        proxy_thread.start()

        client: socket.socket | None = None
        for _attempt in range(20):
            try:
                client = socket.create_connection(("127.0.0.1", proxy_port), timeout=1.0)
                break
            except ConnectionRefusedError:
                time.sleep(0.05)

        self.assertIsNotNone(client)
        assert client is not None
        with client:
            client.settimeout(1.0)
            self.assertEqual(client.recv(1), b"")

        proxy.stop()
        proxy_thread.join(timeout=2.0)
        upstream_listener.close()
        self.assertFalse(proxy_thread.is_alive())


if __name__ == "__main__":
    unittest.main()
