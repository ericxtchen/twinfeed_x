"""Run paced TwinFeed X workers on loopback. CSV goes to stdout, progress to stderr."""
import argparse
from contextlib import contextmanager
import csv
import os
from pathlib import Path
import platform
import select
import subprocess
import sys


@contextmanager
def worker(binary, cpu):
    command = [str(binary.resolve())]
    if cpu is not None:
        command = ["taskset", "-c", str(cpu), *command]
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as child:
        try:
            yield child
        finally:
            if child.poll() is None:
                child.kill()


def send(child, *fields):
    print(*fields, file=child.stdin, flush=True)


def ready(child):
    if not select.select([child.stdout], [], [], 10)[0]:
        raise TimeoutError(f"worker did not become ready: {child.args}")
    fields = child.stdout.readline().split()
    if not fields:
        raise RuntimeError(f"worker exited during startup: {child.args}")
    return [int(field) for field in fields]


def finish(child, timeout):
    output, _ = child.communicate(timeout=timeout)
    if child.returncode:
        raise RuntimeError(f"{child.args} exited with status {child.returncode}")
    # Workers print key/value lines once, after timing stops. Empty values mean no samples.
    metrics = {}
    for line in output.splitlines():
        key, _, value = line.partition(" ")
        metrics[key] = value
    return metrics


def run_trial(args, rate, mode, repeat):
    config = (rate, args.warmup, args.seconds, args.drain_ms, int(mode == "ab"), int(not args.counters_only))
    timeout = args.warmup + args.seconds + args.drain_ms / 1000 + 10

    # Bind the publisher replay port first. The receiver then chooses the two live
    # ports and a common start time. No control packets enter the measured sockets.
    with worker(args.bin_dir / "udp_bench_publisher", args.publisher_cpu) as pub:
        send(pub, *config)
        replay_port, = ready(pub)
        with worker(args.bin_dir / "udp_bench_receiver", args.receiver_cpu) as rx:
            send(rx, *config, replay_port)
            a, b, start = ready(rx)
            send(pub, a, b, start)
            receiver = finish(rx, timeout)
            publisher = finish(pub, timeout)

    return summarize(args, rate, mode, repeat, publisher, receiver)


def summarize(args, rate, mode, repeat, publisher, receiver):
    n = rate * args.seconds
    warmup = rate * args.warmup
    values = {**publisher, **receiver}

    def count(key):
        return int(values[key])

    # These differences stay signed. A missing duplicate is measurable even when
    # the observer saw every logical update and the book is current.
    missing_a = count("sent_a") - count("read_a")
    missing_b = count("sent_b") - count("read_b")
    in_window = count("commits_in_window")
    final = count("commits_after_drain")
    cpu = count("receiver_cpu_window_ns")
    flags = []
    if count("publish_attempts") != n:
        flags.append("publisher_short")
    if count("send_errors") or count("warmup_send_errors"):
        flags.append("send_errors")
    if (count("warmup_read_a"), count("warmup_read_b"), count("warmup_commits")) != (
            warmup, warmup if mode == "ab" else 0, warmup):
        flags.append("warmup_incomplete")
    if missing_a or missing_b:
        flags.append("unaccounted_live")
    if (count("sent_a"), count("sent_b")) != (n, n if mode == "ab" else 0):
        flags.append("offered_load_short")
    if count("replay_requests_all") or count("gaps_opened_all"):
        flags.append("gap_observed")
    if final != n or count("last_sequence") != warmup + n:
        flags.append("incomplete")
    if (values["state"] != "running" or count("gap_at_end") or count("sequence_errors_all")
            or count("unexpected_frames_all") or count("resets_all") != 1 or not count("book_ok")):
        flags.append("receiver_unhealthy")
    if final > in_window:
        flags.append("drained_late")

    return {
        "trial": repeat, "mode": mode, "target_events_s": rate, "seconds": args.seconds,
        "warmup_s": args.warmup, "drain_ms": args.drain_ms, "timed": int(not args.counters_only),
        "publisher_cpu": args.publisher_cpu, "receiver_cpu": args.receiver_cpu, "kernel": platform.release(),
        "outcome": "+".join(flags) or "clean", "scheduled": n,
        "published_events_s": count("publish_attempts") / args.seconds,
        "sent_live_packets_s": count("sent_in_window") / args.seconds,
        "commits_s": in_window / args.seconds, "commits_in_drain": final - in_window,
        "sent_minus_read_a": missing_a, "sent_minus_read_b": missing_b,
        "rx_cpu_pct": round(100 * cpu / (args.seconds * 1e9), 3),
        "rx_cpu_ns_per_window_commit": round(cpu / in_window, 2) if in_window else "",
        **values,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, default=Path(__file__).resolve().parents[2] / "build/release")
    parser.add_argument("--rates", "--rate", type=int, nargs="+", default=[50_000], metavar="EVENTS_PER_SEC")
    parser.add_argument("--mode", choices=["a", "ab", "both"], default="ab")
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=2, help="warm-up seconds")
    parser.add_argument("--drain-ms", type=int, default=1000)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--publisher-cpu", type=int)
    parser.add_argument("--receiver-cpu", type=int)
    parser.add_argument("--counters-only", action="store_true", help="skip latency sampling")
    args = parser.parse_args()
    if (min(args.rates) < 1 or max(args.rates) > 1_000_000_000 or args.seconds < 1
            or args.repeat < 1 or args.warmup < 0 or args.drain_ms < 0):
        parser.error("check the rate, duration, warm-up, drain, and repeat settings")
    allowed = os.sched_getaffinity(0)
    for cpu in (args.publisher_cpu, args.receiver_cpu):
        if cpu is not None and cpu not in allowed:
            parser.error(f"CPU {cpu} is outside the available CPU set {sorted(allowed)}")

    # Swap A and A+B between repeats so one mode doesn't always get the colder run.
    writer = None
    for rate in args.rates:
        for repeat in range(1, args.repeat + 1):
            modes = ["a", "ab"] if args.mode == "both" else [args.mode]
            if repeat % 2 == 0:
                modes.reverse()
            for mode in modes:
                row = run_trial(args, rate, mode, repeat)
                if writer is None:
                    writer = csv.DictWriter(sys.stdout, fieldnames=row)
                    writer.writeheader()
                writer.writerow(row)
                sys.stdout.flush()
                print(f"{mode} @ {rate}/s: {row['outcome']}, {row['commits_in_window']}/{row['scheduled']} "
                      f"commits in window, reads A/B {row['read_a']}/{row['read_b']}, "
                      f"observer p99 {row['scheduled_to_observer_p99_ns'] or 'n/a'} ns, "
                      f"publisher lateness p99 {row['publish_lateness_p99_ns'] or 'n/a'} ns, "
                      f"RX CPU {row['rx_cpu_pct']}%",
                      file=sys.stderr)


if __name__ == "__main__":
    main()
