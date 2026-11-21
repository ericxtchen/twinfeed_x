#include "../include/order_book.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace {
constexpr std::int64_t first_price = 10'000;
constexpr std::uint32_t seed = 67;
constexpr std::size_t updates = 1'000'000;
constexpr std::size_t pairs = 500'000;
constexpr std::array<std::size_t, 4> depths{16, 64, 256, 1024};
constexpr std::size_t position_depth = 1024;
constexpr std::size_t window_width = 64;
static_assert(window_width <= position_depth);

#if defined(TWINFEED_USE_FLAT_MAP) && TWINFEED_USE_FLAT_MAP
constexpr auto container_name = "flat_map";
#else
constexpr auto container_name = "map";
#endif

std::int64_t price_at(std::size_t index) {
    return first_price + static_cast<std::int64_t>(index);
}
      
Message make_message(std::uint64_t seq, std::size_t index,
                     std::uint64_t quantity, Operation operation) {
    return Message{.session_id = 1, .seq_num = seq, .price_ticks = price_at(index),
                   .quantity = quantity, .instrument_id = 1, .side = Side::BID,
                   .operation = operation};
}

double replay_ns(OrderBook& book, const std::vector<Message>& trace) {
    const auto start = std::chrono::steady_clock::now();
    for (const auto& message : trace) {
        book.apply(message);
    }
    const auto stop = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(stop - start).count();
}

void print_row(std::string_view workload, std::size_t depth, std::size_t units,
               double elapsed_ns) {
    std::cout << container_name << ',' << workload << ',' << depth << ',' << seed
              << ',' << units << ',' << elapsed_ns / static_cast<double>(units) << '\n';
}

bool run_upsert(std::size_t depth) {
    OrderBook book;
    std::uint64_t seq = 1;
    std::vector<std::uint64_t> expected(depth, 1);
    for (std::size_t i = 0; i < depth; ++i) {
        book.apply(make_message(seq++, i, 1, Operation::UPSERT_LEVEL));
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, depth - 1);
    for (std::size_t i = 0; i < 10'000; ++i) {
        const auto index = pick(rng);
        book.apply(make_message(seq++, index, 2, Operation::UPSERT_LEVEL));
        expected[index] = 2;
    }

    std::vector<Message> trace;
    trace.reserve(updates);
    for (std::size_t i = 0; i < updates; ++i) {
        const auto index = pick(rng);
        const auto quantity = static_cast<std::uint64_t>(i) + 3;
        trace.push_back(make_message(seq++, index, quantity, Operation::UPSERT_LEVEL));
        expected[index] = quantity;
    }

    const double elapsed = replay_ns(book, trace);
    for (std::size_t i = 0; i < depth; ++i) {
        if (book.quantity_at(Side::BID, price_at(i)) != expected[i]) {
            std::cerr << "Upsert verification failed at price " << price_at(i) << '\n';
            return false;
        }
    }
    print_row("existing_bid_upsert", depth, updates, elapsed);
    return true;
}

// The window controls which slots can change. All depth slots stay in the book
// at pair boundaries, including the untouched slots outside the window.
bool run_churn(std::size_t depth, std::size_t first_slot, std::size_t width,
               std::string_view label) {
    OrderBook book;
    std::uint64_t seq = 1;
    std::vector<unsigned> active(depth, 0);
    for (std::size_t slot = 0; slot < depth; ++slot) {
        book.apply(make_message(seq++, 2 * slot, 1, Operation::UPSERT_LEVEL));
    }

    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> pick_offset(0, width - 1);
    auto next_pair = [&]() {
        const auto slot = first_slot + pick_offset(rng);
        const auto old_offset = active[slot];
        const auto new_offset = 1u - old_offset;
        auto remove = make_message(seq++, 2 * slot + old_offset, 0, Operation::DELETE_LEVEL);
        auto insert = make_message(seq++, 2 * slot + new_offset, 1, Operation::UPSERT_LEVEL);
        active[slot] = new_offset;
        return std::pair{remove, insert};
    };

    for (std::size_t i = 0; i < 5'000; ++i) {
        const auto [remove, insert] = next_pair();
        book.apply(remove);
        book.apply(insert);
    }

    std::vector<Message> trace;
    trace.reserve(2 * pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        const auto [remove, insert] = next_pair();
        trace.push_back(remove);
        trace.push_back(insert);
    }

    const double elapsed = replay_ns(book, trace);
    for (std::size_t slot = 0; slot < depth; ++slot) {
        const auto occupied = price_at(2 * slot + active[slot]);
        const auto absent = price_at(2 * slot + (1u - active[slot]));
        if (book.quantity_at(Side::BID, occupied) != 1u ||
            book.quantity_at(Side::BID, absent).has_value()) {
            std::cerr << label << " verification failed at slot " << slot << '\n';
            return false;
        }
    }
    print_row(label, depth, pairs, elapsed);
    return true;
}
} // namespace

int main(int argc, char* argv[]) {
    const std::string_view mode = argc > 1 ? argv[1] : "existing-upsert";
    if (argc > 2 || (mode != "existing-upsert" && mode != "churn" &&
                     mode != "churn-low" && mode != "churn-high")) {
        std::cerr << "Usage: book_bench [existing-upsert|churn|churn-low|churn-high]\n";
        return 1;
    }

    if (mode == "churn-low" || mode == "churn-high") {
        const auto first = mode == "churn-low" ? 0 : position_depth - window_width;
        return run_churn(position_depth, first, window_width, mode) ? 0 : 1;
    }

    for (const auto depth : depths) {
        const bool ok = mode == "churn"
            ? run_churn(depth, 0, depth, "bid_delete_insert_pair")
            : run_upsert(depth);
        if (!ok) return 1;
    }
    return 0;
}


