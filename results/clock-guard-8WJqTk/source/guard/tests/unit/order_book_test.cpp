#include "order_book.h"
#include "protocol.h"

#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Unlike assert(), these checks stay active in a release build.
void check(bool ok, std::string_view expression,
           std::source_location where = std::source_location::current()) {
    if (!ok) {
        std::ostringstream out;
        out << where.file_name() << ':' << where.line() << ": " << expression;
        throw std::runtime_error(out.str());
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression)

template <typename Function>
void context(const std::string& description, Function function) {
    try { function(); }
    catch (const std::exception& error) {
        throw std::runtime_error(description + ": " + error.what());
    }
}

void expect_level(const std::optional<Level>& actual,
                  const std::optional<Level>& expected) {
    CHECK(actual.has_value() == expected.has_value());
    if (expected) {
        CHECK(actual->price_ticks == expected->price_ticks);
        CHECK(actual->quantity == expected->quantity);
    }
}

void expect_best(const OrderBook& book, Side side, std::int64_t price,
                 std::uint64_t quantity) {
    expect_level(side == Side::BID ? book.best_bid() : book.best_ask(),
                 Level{price, quantity});
}

void expect_empty_side(const OrderBook& book, Side side) {
    CHECK(!(side == Side::BID ? book.best_bid() : book.best_ask()));
}

// This driver satisfies OrderBook::apply's preconditions: one instrument and
// session per driver, strictly increasing sequences, and validated messages.
struct Input {
    OrderBook& book;
    std::uint32_t instrument = 7;
    std::uint64_t next_sequence = 1;

    Message send(Side side, Operation operation, std::int64_t price,
                 std::uint64_t quantity) {
        Message message{42, next_sequence++, price, quantity, instrument, side, operation};
        CHECK(validate(message).has_value());
        book.apply(message);
        return message;
    }
    Message upsert(Side side, std::int64_t price, std::uint64_t quantity) {
        return send(side, Operation::UPSERT_LEVEL, price, quantity);
    }
    Message erase(Side side, std::int64_t price) {
        return send(side, Operation::DELETE_LEVEL, price, 0);
    }
};

void empty_book() {
    OrderBook book;
    CHECK(!book.best_bid());
    CHECK(!book.best_ask());
    CHECK(!book.quantity_at(Side::BID, 10000));
    CHECK(!book.quantity_at(Side::ASK, 10000));
}

void bid_only_lookup_regression() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::BID, 10000, 50);
    // Catches checking asks_.contains() in the BID branch.
    CHECK(book.quantity_at(Side::BID, 10000) == 50);
    CHECK(!book.quantity_at(Side::ASK, 10000));
    expect_best(book, Side::BID, 10000, 50);
}

void ask_only_bid_query_regression() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::ASK, 10002, 30);
    // Must return nullopt, not try bids_.at() and throw std::out_of_range.
    CHECK(!book.quantity_at(Side::BID, 10002));
    CHECK(book.quantity_at(Side::ASK, 10002) == 30);
    expect_empty_side(book, Side::BID);
}

void ask_lookup_and_absent_price() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::ASK, 10002, 30);
    CHECK(book.quantity_at(Side::ASK, 10002) == 30);
    CHECK(!book.quantity_at(Side::ASK, 10001));
    CHECK(!book.quantity_at(Side::ASK, 10003));
}

void same_price_sides_are_independent() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::BID, 10000, 50);
    input.upsert(Side::ASK, 10000, 30);
    CHECK(book.quantity_at(Side::BID, 10000) == 50);
    CHECK(book.quantity_at(Side::ASK, 10000) == 30);
    input.upsert(Side::BID, 10000, 7);
    expect_best(book, Side::BID, 10000, 7);
    expect_best(book, Side::ASK, 10000, 30);
    input.erase(Side::BID, 10000);
    CHECK(!book.quantity_at(Side::BID, 10000));
    CHECK(book.quantity_at(Side::ASK, 10000) == 30);
    expect_empty_side(book, Side::BID);
    expect_best(book, Side::ASK, 10000, 30);
}

void highest_bid_wins() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::BID, 10000, 10);
    expect_best(book, Side::BID, 10000, 10);
    input.upsert(Side::BID, 9999, 20);
    expect_best(book, Side::BID, 10000, 10);
    input.upsert(Side::BID, 10001, 30);
    expect_best(book, Side::BID, 10001, 30);
    CHECK(book.quantity_at(Side::BID, 9999) == 20);
}

void lowest_ask_wins() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::ASK, 10002, 10);
    expect_best(book, Side::ASK, 10002, 10);
    input.upsert(Side::ASK, 10003, 20);
    expect_best(book, Side::ASK, 10002, 10);
    input.upsert(Side::ASK, 10001, 30);
    expect_best(book, Side::ASK, 10001, 30);
    CHECK(book.quantity_at(Side::ASK, 10003) == 20);
}

void upsert_replaces_quantity() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        input.upsert(side, 10000, 10);
        input.upsert(side, 10000, 7);
        CHECK(book.quantity_at(side, 10000) == 7); // Not 17.
        expect_best(book, side, 10000, 7);
        input.upsert(side, 10000, 90);
        CHECK(book.quantity_at(side, 10000) == 90);
        expect_best(book, side, 10000, 90);
    }
}

void delete_best_reveals_next() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        input.upsert(side, 9999, 20);
        input.upsert(side, 10000, 30);
        input.upsert(side, 10001, 40);
        const auto best = side == Side::BID ? 10001 : 9999;
        input.erase(side, best);
        CHECK(!book.quantity_at(side, best));
        expect_best(book, side, 10000, 30);
    }
}

void delete_nonbest_preserves_best() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        input.upsert(side, 9999, 20);
        input.upsert(side, 10000, 30);
        input.upsert(side, 10001, 40);
        input.erase(side, 10000);
        CHECK(!book.quantity_at(side, 10000));
        expect_best(book, side, side == Side::BID ? 10001 : 9999,
                    side == Side::BID ? 40 : 20);
    }
}

void delete_last_level_empties_side() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        const auto other = side == Side::BID ? Side::ASK : Side::BID;
        input.upsert(side, 10000, 10);
        input.upsert(other, 10002, 20);
        input.erase(side, 10000);
        expect_empty_side(book, side);
        CHECK(!book.quantity_at(side, 10000));
        expect_best(book, other, 10002, 20);
    }
}

void missing_deletes_are_noops() {
    OrderBook book;
    Input input{book};
    input.erase(Side::BID, 10000);
    input.erase(Side::ASK, 10000);
    CHECK(!book.best_bid());
    CHECK(!book.best_ask());
    input.upsert(Side::BID, 9999, 11);
    input.upsert(Side::ASK, 10001, 12);
    input.erase(Side::BID, 10001); // That price exists only on the other side.
    input.erase(Side::ASK, 9999);
    expect_best(book, Side::BID, 9999, 11);
    expect_best(book, Side::ASK, 10001, 12);
}

void repeated_absolute_updates() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        for (int i = 0; i < 3; ++i) {
            input.upsert(side, 10000, 25);
            CHECK(book.quantity_at(side, 10000) == 25);
        }
        for (int i = 0; i < 3; ++i) {
            input.erase(side, 10000);
            CHECK(!book.quantity_at(side, 10000));
            expect_empty_side(book, side);
        }
    }
}

void delete_then_reinsert() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        input.upsert(side, 10000, 25);
        input.erase(side, 10000);
        expect_empty_side(book, side);
        input.upsert(side, 10000, 8);
        CHECK(book.quantity_at(side, 10000) == 8);
        expect_best(book, side, 10000, 8);
    }
}

void queries_do_not_create_levels() {
    OrderBook book;
    Input input{book};
    const OrderBook& view = book;
    CHECK(!view.quantity_at(Side::BID, 10001));
    CHECK(!view.quantity_at(Side::ASK, 9999));
    CHECK(!view.best_bid());
    CHECK(!view.best_ask());
    input.upsert(Side::BID, 10000, 10);
    input.upsert(Side::ASK, 10002, 20);
    CHECK(!view.quantity_at(Side::BID, 10001));
    CHECK(!view.quantity_at(Side::ASK, 9999));
    expect_best(view, Side::BID, 10000, 10);
    expect_best(view, Side::ASK, 10002, 20);
}

void sparse_and_large_prices() {
    constexpr std::int64_t high = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t above_u32 = 0x100000000LL;
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        input.upsert(side, high, 3);
        input.upsert(side, 1, 1);
        input.upsert(side, above_u32, 2);
        CHECK(book.quantity_at(side, high) == 3);
        CHECK(book.quantity_at(side, above_u32) == 2);
        CHECK(book.quantity_at(side, 1) == 1);
        expect_best(book, side, side == Side::BID ? high : 1,
                    side == Side::BID ? 3 : 1);
        input.erase(side, high);
        input.erase(side, 1);
        expect_best(book, side, above_u32, 2);
    }
}

void quantity_boundaries() {
    for (const auto side : {Side::BID, Side::ASK}) {
        OrderBook book;
        Input input{book};
        for (const auto quantity : std::array<std::uint64_t, 5>{
                 1, 0xFFFFFFFFULL, 0x100000000ULL, 0x8000000000000000ULL,
                 std::numeric_limits<std::uint64_t>::max()}) {
            input.upsert(side, 10000, quantity);
            CHECK(book.quantity_at(side, 10000) == quantity);
            expect_best(book, side, 10000, quantity);
        }
        input.upsert(side, 10000, 1);
        expect_best(book, side, 10000, 1);
    }
}

void separate_books_are_independent() {
    OrderBook first;
    OrderBook second;
    Input input_a{first, 7};
    Input input_b{second, 8};
    input_a.upsert(Side::BID, 10000, 10);
    input_b.upsert(Side::BID, 10000, 30);
    expect_best(first, Side::BID, 10000, 10);
    expect_best(second, Side::BID, 10000, 30);
    input_a.erase(Side::BID, 10000);
    CHECK(!first.best_bid());
    expect_best(second, Side::BID, 10000, 30);
}

void documented_bid_trace() {
    OrderBook book;
    Input input{book};
    input.upsert(Side::BID, 10000, 10);
    expect_best(book, Side::BID, 10000, 10);
    input.upsert(Side::BID, 9999, 20);
    expect_best(book, Side::BID, 10000, 10);
    CHECK(book.quantity_at(Side::BID, 9999) == 20);
    input.upsert(Side::BID, 10000, 7);
    expect_best(book, Side::BID, 10000, 7);
    input.erase(Side::BID, 10000);
    expect_best(book, Side::BID, 9999, 20);
    CHECK(!book.quantity_at(Side::BID, 10000));
    input.erase(Side::BID, 9999);
    CHECK(!book.best_bid());
    CHECK(!book.quantity_at(Side::BID, 9999));
    CHECK(!book.best_ask());
}

// Independent test oracle: an UNSORTED vector, with linear scans. It neither
// calls OrderBook nor uses std::map or production update/query helpers.
struct LinearModel {
    struct Entry { Side side; std::int64_t price; std::uint64_t quantity; };
    std::vector<Entry> entries;

    void apply(const Message& message) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->side == message.side && it->price == message.price_ticks) {
                if (message.operation == Operation::DELETE_LEVEL) entries.erase(it);
                else it->quantity = message.quantity;
                return;
            }
        }
        if (message.operation == Operation::UPSERT_LEVEL)
            entries.push_back({message.side, message.price_ticks, message.quantity});
    }

    std::optional<std::uint64_t> quantity(Side side, std::int64_t price) const {
        for (const auto& entry : entries)
            if (entry.side == side && entry.price == price) return entry.quantity;
        return std::nullopt;
    }

    std::optional<Level> best(Side side) const {
        std::optional<Level> result;
        for (const auto& entry : entries) {
            if (entry.side != side) continue;
            if (!result || (side == Side::BID ? entry.price > result->price_ticks
                                            : entry.price < result->price_ticks))
                result = Level{entry.price, entry.quantity};
        }
        return result;
    }
};

void deterministic_model_replay() {
    constexpr std::array<std::uint64_t, 4> seeds{1, 42, 0xB00CULL, 0x5457494EULL};
    // Both dense prices and gaps much larger than 32 bits. Every possible
    // generated price on BOTH sides is queried after EVERY operation.
    constexpr std::array<std::int64_t, 32> prices{
        1, 255, 256, 65535, 65536, 0x7FFFFFFFLL, 0x80000000LL,
        0xFFFFFFFFLL, 0x100000000LL, std::numeric_limits<std::int64_t>::max(),
        10000, 10001, 10002, 10003, 10004, 10005, 10006, 10007, 10008, 10009,
        10010, 10011, 10012, 10013, 10014, 10015, 10016, 10017, 10018, 10019,
        10020, 10021
    };
    for (const auto seed : seeds) {
        std::mt19937_64 random(seed);
        OrderBook book;
        Input input{book};
        LinearModel model;
        for (std::size_t step = 0; step < 1000; ++step) {
            const auto side = (random() & 1U) == 0 ? Side::BID : Side::ASK;
            const auto price = prices[static_cast<std::size_t>(random() % prices.size())];
            const bool deleting = random() % 4 == 0;
            auto quantity = random();
            if (quantity == 0) quantity = 1;
            if (deleting) quantity = 0;
            context("seed=" + std::to_string(seed) + " step=" + std::to_string(step) +
                    " side=" + (side == Side::BID ? "BID" : "ASK") +
                    " price=" + std::to_string(price) +
                    " quantity=" + std::to_string(quantity), [&] {
                const auto message = input.send(side, deleting ? Operation::DELETE_LEVEL
                                                              : Operation::UPSERT_LEVEL,
                                                price, quantity);
                model.apply(message);
                expect_level(book.best_bid(), model.best(Side::BID));
                expect_level(book.best_ask(), model.best(Side::ASK));
                for (const auto query_side : {Side::BID, Side::ASK}) {
                    for (const auto query_price : prices) {
                        context("query side=" + std::string(query_side == Side::BID ? "BID" : "ASK") +
                                " price=" + std::to_string(query_price), [&] {
                            CHECK(book.quantity_at(query_side, query_price) ==
                                  model.quantity(query_side, query_price));
                        });
                    }
                    // These positive prices are never generated.
                    CHECK(!book.quantity_at(query_side, 2));
                    CHECK(!book.quantity_at(query_side, 10022));
                }
            });
        }
    }
}

struct Test { std::string_view name; void (*run)(); };
constexpr Test tests[]{
    {"empty_book", empty_book},
    {"bid_only_lookup_regression", bid_only_lookup_regression},
    {"ask_only_bid_query_regression", ask_only_bid_query_regression},
    {"ask_lookup_and_absent_price", ask_lookup_and_absent_price},
    {"same_price_sides_are_independent", same_price_sides_are_independent},
    {"highest_bid_wins", highest_bid_wins},
    {"lowest_ask_wins", lowest_ask_wins},
    {"upsert_replaces_quantity", upsert_replaces_quantity},
    {"delete_best_reveals_next", delete_best_reveals_next},
    {"delete_nonbest_preserves_best", delete_nonbest_preserves_best},
    {"delete_last_level_empties_side", delete_last_level_empties_side},
    {"missing_deletes_are_noops", missing_deletes_are_noops},
    {"repeated_absolute_updates", repeated_absolute_updates},
    {"delete_then_reinsert", delete_then_reinsert},
    {"queries_do_not_create_levels", queries_do_not_create_levels},
    {"sparse_and_large_prices", sparse_and_large_prices},
    {"quantity_boundaries", quantity_boundaries},
    {"separate_books_are_independent", separate_books_are_independent},
    {"documented_bid_trace", documented_bid_trace},
    {"deterministic_model_replay", deterministic_model_replay}
};

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) { std::cerr << "Usage: order_book_tests [case-name]\n"; return 2; }
    const std::string_view selected = argc == 2 ? argv[1] : "";
    std::size_t count = 0;
    std::size_t failures = 0;
    for (const auto& test : tests) {
        if (argc == 2 && selected != test.name) continue;
        ++count;
        try { test.run(); std::cout << "[PASS] " << test.name << '\n'; }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
        catch (...) { ++failures; std::cerr << "[FAIL] " << test.name << ": unexpected exception\n"; }
    }
    if (count == 0) { std::cerr << "Unknown test: " << selected << '\n'; return 2; }
    std::cout << count - failures << '/' << count << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
