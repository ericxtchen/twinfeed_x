# Paced loopback UDP baseline

Run from the project root on Linux. Each trial launches a fresh publisher and receiver on loopback ports chosen by the kernel. Use:

```bash
cmake --preset release
cmake --build --preset release --target udp_bench_publisher udp_bench_receiver
python3 benchmarks/udp_baseline/run.py --publisher-cpu <publisher_cpu_core> --receiver-cpu <receiver_cpu_core> > baseline.csv
```
A logical event is an update to the order book.
The default trial warms up for 2 seconds, then offers 50,000 logical events/s for 10 seconds on A+B. A complete measured stream has 500,000 updates and 500,000 datagrams on each leg. Warm-up sequences populate the same book and have separate counts. Read the summary on the terminal and keep the CSV for analysis.

For a rate sweep, run:

```bash
python3 benchmarks/udp_baseline/run.py --mode both --rates 10000 25000 50000 100000 \
  --repeat 5 --publisher-cpu <publisher_cpu_core> --receiver-cpu <receiver_cpu_core> > sweep.csv
```

Use `--counters-only` at a representative rate to measure the cost of sampling. It keeps the window and send counters. Use `--help` for the other settings. Record the git revision, compiler and power settings with the CSV.

## Following one event

1. In `publisher.cpp`, each sequence has an absolute due time. The publisher drives one instrument across 16 bid and 16 ask levels. A-only suppresses the B send; A+B sends the identical encoded frame twice. Successful sends and send errors are counted at the socket. The last 50 µs of each wait is a spin on the publisher CPU.
2. In `receiver.cpp`, `on_read` sits just after the real UDP receive call. It counts A, B and replay independently, including copies that the arbiter will discard. A sampled sequence keeps its first read timestamp as it moves through the reorder buffer.
3. `on_notice` counts ordered commits, gaps and resets. It timestamps the tail of the update callback. At the end of the trial the receiver checks the quantities on all 32 levels against its committed prefix.

## Reading a row

| Fields | Meaning |
|---|---|
| `scheduled`, `publish_attempts`, `publish_lateness_p99_ns` | Target, number of measured publications actually started, and lateness against the original schedule. The publisher stops starting new events at the end of the window. `starts_at_least_one_event_late` counts events whose successor was already due when publication began. |
| `sent_a/b`, `read_a/b`, `sent_minus_read_a/b` | Per-leg totals for measured sequence numbers through the drain. Compare send and read on each leg even if the observer received every update. |
| `batch_a_1` … `batch_a_8` (also `batch_b_*`) | Bin k counts successful live reads returning k datagrams during measured receive turns. `_calls` sums the bins, `_packets` sums k times each bin, and `_mean` gives packets per successful call. An idle leg has a blank mean. A turn that crosses a phase boundary stays in the phase where it started. |
| `commits_s`, `commits_in_window`, `commits_after_drain` | Throughput uses only measured updates committed within the fixed window. The last field includes catch-up. The receiver allows up to one extra second by default, including when UDP falls silent. |
| `scheduled_to_observer_*_ns` | Scheduled publication to update callback on the same host. Includes sender lateness, queueing and receiver work. |
| `ingest_to_observer_*_ns` | First userspace read of a generated, valid frame to its update callback. Includes waiting for missing predecessors. A duplicate leaves the timestamp intact. |
| `rx_cpu_pct`, `rx_cpu_ns_per_window_commit` | Single receiver thread's CPU time between the measurement boundaries. Divide by the window length or the number of in-window commits respectively. |
| `outcome` | `clean` means the offered stream, both configured legs, book and observer all reconcile, with all measured commits in the window. Other labels identify the exceptions and can occur together. |

Percentiles are nearest-rank over up to 65,536 reproducibly sampled sequences per trial. The two workers select the same sequences. Latency percentiles include only samples that reach their endpoint; read the sample and completion counts with them. Fields ending in `_all` include warm-up. Summarize each repeat separately before comparing runs.

Loss injection is off. If the receiver requests replay, the publisher serves retained frames after finishing the live send loop, as in the demo. Such a row is flagged. A gap or a missing redundant copy therefore stays visible in the rate sweep.
