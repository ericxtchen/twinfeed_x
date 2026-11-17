# TwinFeed X

An experimental C++23 market-data receiver investigating
correctness, fault recovery, and performance in redundant,
latency-sensitive data streams.

The project aims to maintain consistent market state from
two redundant UDP feeds despite packet loss, duplication,
and reordering, while evaluating the performance trade-offs
of different receiver implementations.

## Current Status

Implemented:

- Fixed-width binary market-data protocol with explicit
  big-endian serialization and message validation.
- Price-level order book with interchangeable std::map
  and std::flat_map implementations.
- 43 unit test cases covering protocol validation,
  order-book behavior, and boundary conditions.
- Deterministic order-book testing against an independent
  linear reference model.
- Order-book microbenchmarks and Linux perf profiling.

Planned:

- End-to-end deterministic receiver replay.
- Sequence validation and redundant UDP feed arbitration.
- Bounded reordering and duplicate suppression.
- Gap recovery and snapshot-based recovery.
- End-to-end latency and throughput characterization.

## Performance Study

The initial study investigates the performance trade-offs
between tree-based and contiguous order-book storage.

At 1,024 active price levels:

- std::flat_map achieved a 2.64x median paired speedup
  for existing-price updates.
- Under delete-insert churn, std::flat_map took 94.7%
  longer than std::map, based on the median paired ratio.
- Low-price churn was 4.55x slower than high-price churn
  within std::flat_map, based on median paired ratios.
- Linux perf attributed 81.16% of low-price churn
  CPU-clock samples to memmove.

These results are specific to the synthetic workloads
and experimental environment described in the report.

[Full benchmarking and profiling study](benchmarks/book_bench.md)

[Raw benchmark results](results/)

## Build and Test

Requirements:

- CMake 3.25+
- Ninja
- A compiler and standard library supporting C++23
- std::flat_map support for the flat-map configuration

Default std::map configuration:

    cmake --preset debug
    cmake --build --preset debug
    ctest --preset debug

AddressSanitizer and UndefinedBehaviorSanitizer:

    cmake --preset asan
    cmake --build --preset asan
    ctest --preset asan

## Run Benchmarks

Build the default tree-based order book:

    cmake --preset release -B build/bench-map
    cmake --build build/bench-map --parallel

Build the contiguous order book:

    cmake --preset release -B build/bench-flat \
      -DTWINFEED_USE_FLAT_MAP=ON
    cmake --build build/bench-flat --parallel

Available workloads:

    ./build/bench-map/book_bench existing-upsert
    ./build/bench-map/book_bench churn
    ./build/bench-map/book_bench churn-low
    ./build/bench-map/book_bench churn-high

Run the same commands using build/bench-flat to
benchmark the flat-map implementation.

## Repository Structure

    include/       Protocol and order-book interfaces
    src/           Core implementations
    tests/         Protocol and order-book unit tests
    benchmarks/    Benchmark harness and experimental report
    results/       Raw measurements and profiling data
    docs/          Protocol specification

## Documentation

- [Wire protocol specification](docs/protocol.md)
- [Benchmarking and profiling study](benchmarks/book_bench.md)

## Scope

TwinFeed X is a work-in-progress research and learning
project. It uses synthetic market-data messages and does
not connect to a real exchange.

Current benchmarks measure isolated order-book update
costs, not end-to-end receiver latency. Those benchmarks will come soon.

