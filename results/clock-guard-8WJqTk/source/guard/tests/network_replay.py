"""Exercise both executables and inspect the actual replay datagrams on loopback."""
from contextlib import contextmanager
import signal
import socket
import struct
import subprocess
import sys


@contextmanager
def running(*argv):
    with subprocess.Popen(argv, stdout=subprocess.PIPE, text=True) as child:
        try:
            yield child.stdout
        finally:
            child.kill()


def expect(actual, expected):
    if actual != expected:
        raise AssertionError(f"expected {expected!r}, received {actual!r}")


def port_line(stream, label, count):
    fields = stream.readline().split()
    expect(fields[:1], [label])
    expect(len(fields), 1 + count)
    return fields[1:]


def test(receiver, publisher):
    # The relay lets us inspect and withhold the reply before the missing event commits.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as relay:
        relay.bind(("127.0.0.1", 0))
        with running(receiver, "0", "0", str(relay.getsockname()[1])) as rx:
            expect(rx.readline().strip(), "reset 7 0")
            a, b = port_line(rx, "live-ports", 2)
            with running(publisher, a, b, "0", "--drop-four") as pub:
                port, = port_line(pub, "replay-port", 1)
                source = ("127.0.0.1", int(port))
                request, requester = relay.recvfrom(65535)
                expect(request, struct.pack("!HQQQ", 1, 7, 4, 4))
                for seq in (1, 2, 3):
                    expect(rx.readline().strip(), f"commit {seq} quantity {seq}")
                expect(rx.readline().strip(), "gap open")
                expect(rx.readline().strip(), "request 4 4")

                relay.sendto(request, source)
                reply, sender = relay.recvfrom(65535)
                expect(sender, source)
                expect(reply, struct.pack("!HQQqQIBB", 1, 7, 4, 100, 4, 101, 0, 0))
                relay.sendto(reply, requester)
                for seq in (4, 5):
                    expect(rx.readline().strip(), f"commit {seq} quantity {seq}")
                expect(rx.readline().strip(), "gap closed")

                # A range is returned as separate unchanged event datagrams.
                relay.sendto(struct.pack("!HQQQ", 1, 7, 2, 4), source)
                for seq in (2, 3, 4):
                    packet, sender = relay.recvfrom(65535)
                    expect(sender, source)
                    expect(packet, struct.pack("!HQQqQIBB", 1, 7, seq, 100, seq, 101, 0, 0))


def timeout(signum, frame):
    raise TimeoutError("timed out waiting for the network replay")


if __name__ == "__main__":
    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(10)
    test(*sys.argv[1:])
