#include "receiver.h"
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr Message msg1{.session_id = 7, .seq_num = 1, .price_ticks = 100, .quantity = 10,
                       .instrument_id = 101, .side = Side::BID, .operation = Operation::UPSERT_LEVEL};
constexpr Message msg2{.session_id = 7, .seq_num = 2, .price_ticks = 105, .quantity = 4,
                       .instrument_id = 202, .side = Side::ASK, .operation = Operation::UPSERT_LEVEL};
constexpr Message msg3{.session_id = 7, .seq_num = 3, .price_ticks = 100, .quantity = 0,
                       .instrument_id = 101, .side = Side::BID, .operation = Operation::DELETE_LEVEL};

struct BookView {
    std::uint64_t committed;
    std::optional<std::uint64_t> bid_101;
    std::optional<std::uint64_t> ask_202;
    bool operator==(const BookView&) const = default;
};

BookView read_book(const Receiver& view) {
    return {view.last_committed_sequence(), view.quantity_at(101, Side::BID, 100),
            view.quantity_at(202, Side::ASK, 105)};
}

struct Update {
    std::uint64_t sequence;
    BookView book_at_callback;
    bool operator==(const Update&) const = default;
};

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

void four_arrivals() {
    constexpr BookView after_1{1, 10, std::nullopt};
    constexpr BookView after_2{2, 10, 4};
    constexpr BookView after_3{3, std::nullopt, 4};
    unsigned resets = 0;
    unsigned notifications = 0;
    std::vector<Update> updates;
    updates.reserve(3);
    Receiver receiver{7, [&](const Receiver& view, const Receiver::Notification& notice) {
        ++notifications;
        if (const auto* msg = std::get_if<Message>(&notice))
            updates.push_back({msg->seq_num, read_book(view)});
        else if (std::holds_alternative<Receiver::Reset>(notice))
            ++resets;
    }};

    auto deliver = [&](Feed feed, const Message& msg) {
        const auto packet = encode(msg).value();
        receiver.on_datagram(feed, packet);
    };
    auto expect_only_1 = [&](const std::string& arrival) {
        require(read_book(receiver) == after_1, arrival + ": expected the book at sequence 1");
        require(resets == 1 && updates.size() == 1 && updates.front() == Update{1, after_1},
                arrival + ": expected only the bootstrap reset and update 1");
    };

    deliver(Feed::A, msg1);
    expect_only_1("A1");
    deliver(Feed::A, msg3);  // 2 is missing; the deletion must wait.
    expect_only_1("A3");
    const auto before_duplicate = notifications;
    deliver(Feed::B, msg3);  // Same 40 bytes, from the other feed.
    expect_only_1("B3");
    require(notifications == before_duplicate, "B3: identical copy emitted a notification");

    deliver(Feed::B, msg2);
    const auto committed = receiver.last_committed_sequence();
    require(committed == 3, "B2: expected committed sequence 3, got " + std::to_string(committed));
    require(resets == 1 && updates.size() == 3, "B2: expected two new updates and no reset");
    require(updates[1] == Update{2, after_2}, "Update(2): expected the bid to still exist");
    require(updates[2] == Update{3, after_3}, "Update(3): expected the bid to have been deleted");
    require(read_book(receiver) == after_3, "B2: incorrect final book");
}

// Each case commits 1, holds a future message, and encounters an occupied slot.
// The final delivery of 2 would advance the book if the terminal state did not latch.
void expect_buffer_failure(const Message& held, const Message& incoming, Receiver::State expected) {
    constexpr BookView after_1{1, 10, std::nullopt};
    std::vector<Receiver::Notification> seen;
    seen.reserve(4);
    auto state_at_status = Receiver::State::Running;
    Receiver receiver{7, [&](const Receiver& view, const Receiver::Notification& notice) {
        if (const auto* status = std::get_if<Receiver::Status>(&notice)) {
            // Ordinary gap transitions can precede the terminal transition under test.
            if (status->state != Receiver::State::NeedsRecovery && status->state != Receiver::State::Error)
                return;
            state_at_status = view.state();
        }
        seen.push_back(notice);
    }};

    receiver.on_datagram(Feed::A, encode(msg1).value());
    receiver.on_datagram(Feed::A, encode(held).value());
    require(receiver.state() == Receiver::State::Running && read_book(receiver) == after_1,
            "holding the first future event should preserve the committed book");
    require(seen.size() == 2 && std::holds_alternative<Receiver::Reset>(seen[0]) &&
            std::get<Message>(seen[1]) == msg1, "expected only reset and update 1");

    receiver.on_datagram(Feed::B, encode(incoming).value());
    require(receiver.state() == expected, "occupied slot: wrong receiver state");
    require(read_book(receiver) == after_1, "occupied slot: committed book changed");
    require(seen.size() == 3, "expected exactly one status notification");
    const auto* status = std::get_if<Receiver::Status>(&seen.back());
    require(status && status->state == expected && state_at_status == expected &&
            status->incoming_sequence == incoming.seq_num, "incorrect status notification");

    receiver.on_datagram(Feed::B, encode(msg2).value());
    require(receiver.state() == expected && read_book(receiver) == after_1 && seen.size() == 3,
            "late gap closure must preserve the terminal state and committed prefix");
}

void slot_collision() {
    Message msg67 = msg3;
    msg67.seq_num = 67;  // Both 67 & 63 and 3 & 63 select slot 3.
    expect_buffer_failure(msg67, msg3, Receiver::State::NeedsRecovery);
}

void conflicting_copy() {
    Message other_copy = msg3;
    ++other_copy.price_ticks;  // Two valid deletes at different prices with sequence 3.
    expect_buffer_failure(msg3, other_copy, Receiver::State::Error);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view name = argc == 1 ? "four_arrivals" : argv[1];
        if (argc > 2) return 2;
        if (name == "four_arrivals") four_arrivals();
        else if (name == "slot_collision") slot_collision();
        else if (name == "conflicting_copy") conflicting_copy();
        else return 2;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
