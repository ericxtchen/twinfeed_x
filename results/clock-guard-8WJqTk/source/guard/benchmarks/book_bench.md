# Benchmarking std::map and std::flat_map across different workloads

## Setup
I ran my benchmarks and profiling on a Lenovo Legion 5 15ARP8 with a Ryzen 7 7735HS on Fedora 45.
An `OrderBook` holds an instrument's quantity at a given bid and ask price. Depth is the number of such unique price: quantity pairs.
I only tested bids, not asks as they are the same data structure and are functionally the same.

In my benchmarks, a new `OrderBook` is instantiated, a random generator is seeded with the number `67`,
and prepopulate the book with `depth` number of unique prices.
Then, we create a uniform distribution and use our random number generator to apply a total of 10,000 updates to our order book.
This is to prepopulate it with data.

Then, we prepare a simulated list of actions to do on the order book using our `rng`, which is the `trace`.
These actions depend on what workload we want to benchmark.

Then, we start a stopwatch with `std::chrono::steady_clock` and time how long is takes to call `OrderBook::apply()` on all the actions in our `trace`.

After timing, we check for correctness and print the results.

The initial population establishes the desired book depth. The subsequent 10,000 untimed updates exercise the update path and reduce cold-start effects before measurement. The benchmark does not explicitly control or flush CPU caches.

Each benchmark consisted of 10 paired trials per depth where we alternate between using `std::map` and std::flat_map`.

## Find + Update workload
The question here was how long does it take to find and update an already existing entry?
Because we have prepopulated every entry, every upsert is guaranteed to hit.
```bash
./build/bench-map/book_bench
./build/bench-flat/book_bench
cat results/existing_bid_upsert_20261116_181744.csv
```
| Active bid prices | Map median ns/update | Flat median ns/update | Paired speedup, map time / flat time | Flat wins |
|---:|---:|---:|---:|---:|
| 16 | 12.55 | 10.56 | 1.20× | 10/10 |
| 64 | 19.90 | 11.14 | 1.79× | 10/10 |
| 256 | 24.59 | 11.86 | 2.09× | 10/10 |
| 1,024 | 36.31 | 13.82 | 2.64× | 10/10 |

Across all 40 comparisons, `std::flat_map` wins. This is not really that surprising as `std::flat_map` is essentially just a sorted contigious array.
Both containers provide logarithmic lookup complexity. However, std::flat_map performs binary search over contiguous key storage, while std::map typically traverses separately allocated tree nodes. Differences in memory layout, locality, and traversal overhead may explain the observed performance advantage. Hardware cache events were not measured in this study.
There is also the advantage of cache locality for contigious arrays, compared to tree nodes that `std::map` uses, but I didn't collect cache metrics in my benchmarks.

## Delete and Insert workload
The question now after doing find + update is if `std::flat_map`'s superiority still exists in this specific workload.
The unit of work is one deletion and then one insertion.
We can define a slot as a pair of neighboring bid prices. A slot `i` maps to indices `2 * i` and `2 * i + 1`.
Depth changes from `N -> N - 1 -> N`. There are a total of 1 million calls to `OrderBook::apply()`.
```bash
cmake --build build/bench-map --target book_bench --parallel
cmake --build build/bench-flat --target book_bench --parallel
./build/bench-map/book_bench churn
./build/bench-flat/book_bench churn
cat results/bid_delete_insert_pair_20261116_182350.csv
```
| Depth | Map median ns/pair | Flat median ns/pair | Paired flat time relative to map | Flat wins |
|---:|---:|---:|---:|---:|
| 16 | 50.65 | 33.76 | 33.1% less | 9/10 |
| 64 | 65.60 | 51.49 | 21.3% less | 9/10 |
| 256 | 77.83 | 87.36 | 12.9% more | 0/10 |
| 1,024 | 94.28 | 182.98 | 94.7% more | 0/10 |
We can see that the higher the depth, the more `std::map` wins.
However, we know that `std::flat_map` is essentially a sorted array, so maybe the reason it loses is because of how many elements it has to shift during deletion and insertion.

## Deleting and Inserting in a low vs high index
Beacuse `std::flat_map` is a sorted contigious array, if are slots map to higher indices, the less elements we have to move during deletion and insertion, and vice versa.

Both cases kept 1,024 live levels at pair boundaries, with 64 eligible slots, 500,000 pairs and the same pseudorandom offsets. Low churn used slots 0–63. High churn added 960 to every offset. When low churn selects slot 7, high churn selects slot 967. The erase therefore shifts 1,016 versus 56 later entries in the flat representation, and insertion shifts them back. The number of eligible slots, operations, and active levels is identical.
```bash
for build in bench-map bench-flat; do
  cmake --build "build/$build" --target book_bench --parallel
  "./build/$build/book_bench" churn-low
  "./build/$build/book_bench" churn-high
done
cat results/churn_position_20261116_204244.csv
```
| Container | Low-price median ns/pair | High-price median ns/pair | Median paired low/high ratio |
|---|---:|---:|---:|
| map | 88.20 | 109.67 | 0.81× |
| flat_map | 277.72 | 61.21 | 4.55× |

We can see that `std::map` loses to `std::flat_map` in high indices and vice versa.
The hypothesis is that with low indices, `std::flat_map` has to do big memory operations like copying, which adds substantial overhead.
Prices are in ascending order.

## Profiling position-dependent data movement in `std::flat_map`
```bash
cmake --preset release -B build/profile-flat -DTWINFEED_USE_FLAT_MAP=ON -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -DNDEBUG"
cmake --build build/profile-flat --target book_bench --parallel
./build/profile-flat/book_bench churn-low

./build/profile-flat/book_bench churn-high
```
We build with `-O3` and debugging symbols.
Now, we can compare both workloads with `perf`.
```bash
for mode in churn-low churn-high; do
  perf report --stdio --no-children --comms book_bench --percent-limit 1 \
    -i "results/profiles/clock-$mode.data" | head -60
done
```
| `perf report` output (template names shortened) | Low churn | High churn |
|---|---:|---:|
| Samples / lost samples | 860 / 0 | 174 / 0 |
| libc: `__memmove_avx_unaligned_erms` | 81.16% | 18.97% |
| flat-map `erase` | 6.63% | 25.86% |
| `OrderBook::apply` | 4.30% | 20.11% |
| flat-map `_M_try_emplace` | 3.49% | 17.82% |
| `run_churn` | 2.21% | 9.20% |

For low churn (low indices) about 698 of 860 samples landed in `memmove` and only 33 samples at high churn.
```bash
perf record -o results/profiles/user-stacks-probe.data -e cpu-clock:u -F 499 --user-callchains --call-graph dwarf -- ./build/profile-flat/book_bench churn-low
perf report --stdio --no-children --call-graph graph,0.5,caller \
  --comms book_bench --symbol-filter memmove \
  -i results/profiles/user-stacks-probe.data | less -S
```

The relevant frames are below:
```
74.84%  __memmove_avx_unaligned_erms
  74.19%  main -> run_churn -> replay_ns (inlined)
    39.35%  flat-map erase
      25.81%  vector<long>::erase          -> move -> memmove
      13.55%  vector<unsigned long>::erase -> move -> memmove
    34.84%  OrderBook::apply → flat_map::operator[] -> insertion
      18.06%  vector<long> insertion          -> move_backward -> memmove
      16.77%  vector<unsigned long> insertion -> move_backward -> memmove
```
Of all samples in this probe, 74.84% landed in memmove, and 74.19% reached it through the timed replay_ns loop. The erase path shifted both the signed price keys and unsigned quantities left. The insertion path shifted both arrays right. This connects the dominant low-price hotspot to maintaining the sorted book during replay. The separate ten-run comparison shows how much more prominent that work was at low prices, where more entries followed the selected slot.

## Conclusion
Because the results were varied across depth and workload, the best idea is simply to just choose the data structure that matches your specific workload.
These were synthetic, single-threaded and single-instrument bid traces with only one `rng` seed. No fancy techniques were used like core pinning.
The study did not measure per-message tails, hardware cache events, or end-to-end receiver latency.

Perhaps there are better data structures than the STL ones or perhaps a combination of data structures could be used for better overall performance.
