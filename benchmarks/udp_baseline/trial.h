#pragma once

#include "protocol.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace bench {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using namespace std::chrono_literals;
using Count = std::uint64_t;

inline std::int64_t ns(Clock::duration duration) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

struct Trial {
    Count rate;
    int warmup_seconds;
    int seconds;
    int drain_ms;
    bool dual;
    bool timed;
    Time start{};

    Count warmup_count() const { return rate * static_cast<Count>(warmup_seconds); }
    Count measured_count() const { return rate * static_cast<Count>(seconds); }
    Count last_sequence() const { return warmup_count() + measured_count(); }
    Time measured_start() const { return start + std::chrono::seconds(warmup_seconds); }
    Time end() const { return measured_start() + std::chrono::seconds(seconds); }
    Time drain_end() const { return end() + std::chrono::milliseconds(drain_ms); }

    // Calculate each deadline from the same origin. A late send doesn't move the next one.
    Time due(Count sequence) const {
        const Count index = sequence - 1;
        return start + std::chrono::seconds(index / rate)
                     + std::chrono::nanoseconds((index % rate) * 1'000'000'000ULL / rate);
    }
};

inline Trial read_trial() {
    Trial t{};
    if (!(std::cin >> t.rate >> t.warmup_seconds >> t.seconds >> t.drain_ms >> t.dual >> t.timed)
        || t.rate == 0 || t.rate > 1'000'000'000 || t.seconds < 1 || t.warmup_seconds < 0 || t.drain_ms < 0)
        throw std::invalid_argument("invalid trial settings from runner");
    return t;
}

// Alternate sides, cycling through 16 prices per side. A revisit replaces the quantity.
inline Message update(Count sequence) {
    const auto level = static_cast<std::int64_t>(((sequence - 1) / 2) % 16);
    const bool bid = sequence % 2 == 1;
    return {7, sequence, bid ? 9999 - level : 10000 + level, 1 + (sequence - 1) / 32, 101,
            bid ? Side::BID : Side::ASK, Operation::UPSERT_LEVEL};
}

// Pick one event in each block up front. Fixed-stride sampling would keep hitting
// the same few book levels at rates where the stride divides 32.
class SamplePlan {
public:
    explicit SamplePlan(const Trial& trial) {
        if (!trial.timed) return;
        const auto count = trial.measured_count();
        constexpr Count max_samples = 65536;
        stride_ = 1 + (count - 1) / max_samples;
        std::mt19937_64 rng(7);
        for (Count begin = 0; begin < count; begin += stride_)
            chosen_.push_back(begin + rng() % std::min(stride_, count - begin));
    }

    std::optional<std::size_t> slot(Count measured_index) const {
        if (chosen_.empty()) return {};
        const auto group = static_cast<std::size_t>(measured_index / stride_);
        if (chosen_[group] == measured_index) return group;
        return {};
    }
    std::size_t size() const { return chosen_.size(); }

private:
    Count stride_ = 1;
    std::vector<Count> chosen_;
};

inline void metric(std::string_view key, auto value) { std::cout << key << ' ' << value << '\n'; }

// Each worker fills its reserved sample vector in the hot path. Sorting happens
// after the sockets have finished. Empty percentiles become blank fields in the CSV.
inline void percentiles(std::string_view name, std::vector<std::int64_t>& samples) {
    std::erase(samples, -1);  // This sequence never reached the observer.
    std::sort(samples.begin(), samples.end());
    std::cout << name << "_samples " << samples.size() << '\n';
    auto print_quantile = [&](std::string_view suffix, std::size_t permille) {
        std::cout << name << suffix << ' ';
        if (!samples.empty()) {
            const auto rank = (samples.size() * permille + 999) / 1000;
            std::cout << samples[rank - 1];
        }
        std::cout << '\n';
    };
    print_quantile("_p50_ns", 500);
    print_quantile("_p99_ns", 990);
    print_quantile("_p999_ns", 999);
}
} // namespace bench
