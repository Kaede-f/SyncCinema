#!/usr/bin/env python3
"""TCP proxy for repeatable SyncCinema latency and interruption experiments."""

from __future__ import annotations

import argparse
import json
import random
import signal
import socket
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone


BUFFER_SIZE = 64 * 1024


@dataclass(frozen=True)
class FaultProfile:
    latency_ms: float
    jitter_ms: float
    stall_after_seconds: float | None
    stall_duration_seconds: float
    disconnect_after_seconds: float | None


def log_event(event: str, **attributes: object) -> None:
    record = {
        "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
        "event": event,
        **attributes,
    }
    print(json.dumps(record, sort_keys=True), flush=True)


class FaultConnection:
    def __init__(
        self,
        connection_id: int,
        client_socket: socket.socket,
        upstream_socket: socket.socket,
        profile: FaultProfile,
        seed: int,
    ) -> None:
        self.connection_id = connection_id
        self.client_socket = client_socket
        self.upstream_socket = upstream_socket
        self.profile = profile
        self.seed = seed
        self.started_at = time.monotonic()
        self.stop_event = threading.Event()
        self._close_lock = threading.Lock()
        self._closed = False

    def run(self) -> None:
        threads = [
            threading.Thread(
                target=self._relay,
                args=(self.client_socket, self.upstream_socket, "client_to_server", 0),
                daemon=True,
            ),
            threading.Thread(
                target=self._relay,
                args=(self.upstream_socket, self.client_socket, "server_to_client", 1),
                daemon=True,
            ),
        ]
        if self.profile.disconnect_after_seconds is not None:
            threads.append(
                threading.Thread(target=self._disconnect_on_deadline, daemon=True)
            )
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.close("relay_finished")

    def _relay(
        self,
        source: socket.socket,
        destination: socket.socket,
        direction: str,
        direction_seed: int,
    ) -> None:
        random_source = random.Random(self.seed + direction_seed)
        bytes_forwarded = 0
        try:
            while not self.stop_event.is_set():
                payload = source.recv(BUFFER_SIZE)
                if not payload:
                    self.close(f"{direction}_eof")
                    return

                self._wait_for_fault_window(random_source)
                destination.sendall(payload)
                bytes_forwarded += len(payload)
        except (ConnectionError, OSError) as error:
            if not self.stop_event.is_set():
                log_event(
                    "relay_error",
                    connection_id=self.connection_id,
                    direction=direction,
                    error=str(error),
                )
        finally:
            log_event(
                "relay_stopped",
                connection_id=self.connection_id,
                direction=direction,
                bytes_forwarded=bytes_forwarded,
            )
            self.close(f"{direction}_stopped")

    def _disconnect_on_deadline(self) -> None:
        deadline = self.profile.disconnect_after_seconds
        if deadline is not None and not self.stop_event.wait(deadline):
            # 独立定时器保证空闲连接也会准时断开，不依赖下一次 recv 返回。
            self.close("scheduled_disconnect")

    def _wait_for_fault_window(self, random_source: random.Random) -> None:
        elapsed = time.monotonic() - self.started_at
        stall_start = self.profile.stall_after_seconds
        if stall_start is not None:
            stall_end = stall_start + self.profile.stall_duration_seconds
            if stall_start <= elapsed < stall_end:
                time.sleep(stall_end - elapsed)

        # 每个方向独立注入抖动；固定 seed 让同一组参数可以复现实验。
        jitter = random_source.uniform(
            -self.profile.jitter_ms,
            self.profile.jitter_ms,
        )
        delay_ms = max(0.0, self.profile.latency_ms + jitter)
        if delay_ms > 0:
            time.sleep(delay_ms / 1000.0)

    def close(self, reason: str) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            self.stop_event.set()
            for connected_socket in (self.client_socket, self.upstream_socket):
                try:
                    connected_socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                connected_socket.close()
            log_event(
                "connection_closed",
                connection_id=self.connection_id,
                reason=reason,
                duration_ms=round((time.monotonic() - self.started_at) * 1000),
            )


class FaultProxy:
    def __init__(
        self,
        listen_host: str,
        listen_port: int,
        upstream_host: str,
        upstream_port: int,
        profile: FaultProfile,
        seed: int,
    ) -> None:
        self.listen_host = listen_host
        self.listen_port = listen_port
        self.upstream_host = upstream_host
        self.upstream_port = upstream_port
        self.profile = profile
        self.seed = seed
        self.stop_event = threading.Event()
        self.listener: socket.socket | None = None
        self._next_connection_id = 1

    def run(self) -> None:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            self.listener = listener
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind((self.listen_host, self.listen_port))
            listener.listen()
            listener.settimeout(1.0)
            log_event(
                "proxy_started",
                listen=f"{self.listen_host}:{self.listen_port}",
                upstream=f"{self.upstream_host}:{self.upstream_port}",
                latency_ms=self.profile.latency_ms,
                jitter_ms=self.profile.jitter_ms,
                stall_after_seconds=self.profile.stall_after_seconds,
                stall_duration_seconds=self.profile.stall_duration_seconds,
                disconnect_after_seconds=self.profile.disconnect_after_seconds,
                seed=self.seed,
            )

            while not self.stop_event.is_set():
                try:
                    client_socket, client_address = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    if self.stop_event.is_set():
                        break
                    raise

                connection_id = self._next_connection_id
                self._next_connection_id += 1
                try:
                    upstream_socket = socket.create_connection(
                        (self.upstream_host, self.upstream_port),
                        timeout=10.0,
                    )
                    upstream_socket.settimeout(None)
                except OSError as error:
                    client_socket.close()
                    log_event(
                        "upstream_connect_failed",
                        connection_id=connection_id,
                        client=f"{client_address[0]}:{client_address[1]}",
                        error=str(error),
                    )
                    continue

                log_event(
                    "connection_opened",
                    connection_id=connection_id,
                    client=f"{client_address[0]}:{client_address[1]}",
                )
                connection = FaultConnection(
                    connection_id,
                    client_socket,
                    upstream_socket,
                    self.profile,
                    self.seed + connection_id * 100,
                )
                threading.Thread(target=connection.run, daemon=True).start()

        log_event("proxy_stopped")

    def stop(self) -> None:
        self.stop_event.set()
        if self.listener is not None:
            self.listener.close()


def non_negative(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def port_number(value: str) -> int:
    parsed = int(value)
    if not 1 <= parsed <= 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return parsed


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=port_number, default=9000)
    parser.add_argument("--upstream-host", required=True)
    parser.add_argument("--upstream-port", type=port_number, default=9000)
    parser.add_argument("--latency-ms", type=non_negative, default=0.0, help="one-way delay per TCP chunk")
    parser.add_argument("--jitter-ms", type=non_negative, default=0.0)
    parser.add_argument("--stall-after-seconds", type=non_negative)
    parser.add_argument("--stall-duration-seconds", type=non_negative, default=0.0)
    parser.add_argument("--disconnect-after-seconds", type=non_negative)
    parser.add_argument("--seed", type=int, default=20261021)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if (
        arguments.listen_host == arguments.upstream_host
        and arguments.listen_port == arguments.upstream_port
    ):
        raise SystemExit("listen endpoint and upstream endpoint must differ")
    if arguments.stall_after_seconds is not None and arguments.stall_duration_seconds <= 0:
        raise SystemExit("--stall-duration-seconds must be positive when a stall is configured")

    profile = FaultProfile(
        latency_ms=arguments.latency_ms,
        jitter_ms=arguments.jitter_ms,
        stall_after_seconds=arguments.stall_after_seconds,
        stall_duration_seconds=arguments.stall_duration_seconds,
        disconnect_after_seconds=arguments.disconnect_after_seconds,
    )
    proxy = FaultProxy(
        arguments.listen_host,
        arguments.listen_port,
        arguments.upstream_host,
        arguments.upstream_port,
        profile,
        arguments.seed,
    )

    def stop_proxy(_signal_number: int, _frame: object) -> None:
        proxy.stop()

    signal.signal(signal.SIGINT, stop_proxy)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, stop_proxy)
    proxy.run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
