"""Exercise the measurement cutoff without relying on the publisher's pacing."""
from pathlib import Path
import socket
import struct
import sys
import time

from run import finish, ready, send, worker


def wait_until(ns):
    time.sleep(max(0, (ns - time.monotonic_ns()) / 1e9))


def check(metrics, **expected):
    for key, value in expected.items():
        if metrics[key] != str(value):
            raise AssertionError(f"{key}: expected {value}, got {metrics[key]}")


def test(binary):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
        source.bind(("127.0.0.1", 0))
        port = source.getsockname()[1]

        # The final B copy arrives during the drain, after A already committed it.
        with worker(binary, None) as rx:
            send(rx, 1, 1, 1, 1500, 1, 1, port)
            a, b, start = ready(rx)
            first = struct.pack("!HQQqQIBB", 1, 7, 1, 9999, 1, 101, 0, 0)
            second = struct.pack("!HQQqQIBB", 1, 7, 2, 10000, 1, 101, 1, 0)
            wait_until(start)
            for dest in (a, b):
                source.sendto(first, ("127.0.0.1", dest))
            wait_until(start + 1_200_000_000)  # Well inside the measured phase.
            source.sendto(second, ("127.0.0.1", a))
            wait_until(start + 2_100_000_000)
            source.sendto(second, ("127.0.0.1", b))
            check(finish(rx, 5), read_a=1, read_b=1, warmup_commits=1, commits_in_window=1,
                  commits_after_drain=1, last_sequence=2, book_ok=1, scheduled_to_observer_samples=1,
                  batch_a_1=1, batch_a_calls=1, batch_a_packets=1, batch_a_mean=1,
                  batch_b_calls=0, batch_b_packets=0, batch_b_mean="")

        # With no datagrams there is no gap timer to wake ppoll. The trial's own
        # deadline must still wake it and produce a row with zero latency samples.
        with worker(binary, None) as rx:
            send(rx, 1, 0, 1, 50, 0, 1, port)
            ready(rx)
            check(finish(rx, 5), read_a=0, read_b=0, commits_after_drain=0,
                  scheduled_to_observer_samples=0, scheduled_to_observer_p99_ns="",
                  batch_a_calls=0, batch_a_mean="", batch_b_calls=0, batch_b_mean="")


if __name__ == "__main__":
    test(Path(sys.argv[1]))
