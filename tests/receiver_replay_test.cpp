#include "receiver.h"
#include "protocol.h"
#include <optional>
#include <span>
#include <string_view>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <vector>

void check(bool ok, std::string_view expression,
           std::source_location where = std::source_location::current()) {
    if (!ok) {
        std::ostringstream out;
        out << where.file_name() << ':' << where.line() << ": " << expression;
        throw std::runtime_error(out.str());
    }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression)

constexpr Message msg1{ 
    .session_id = 7, 
    .seq_num = 1,
    .price_ticks = 100,
    .quantity = 10,
    .instrument_id = 101, 
    .side = Side::BID,
    .operation = Operation::UPSERT_LEVEL,
};

constexpr Message msg2{ 
    .session_id = 7, 
    .seq_num = 2,
    .price_ticks = 105,
    .quantity = 4,
    .instrument_id = 202, 
    .side = Side::ASK,
    .operation = Operation::UPSERT_LEVEL,
};

constexpr Message msg3{ 
    .session_id = 7, 
    .seq_num = 3,
    .price_ticks = 100,
    .quantity = 0,
    .instrument_id = 101, 
    .side = Side::BID,
    .operation = Operation::DELETE_LEVEL,
};
int main() {
    struct Observed {
        Receiver::Notification notification;
        std::uint64_t committed;
        std::optional<std::uint64_t> quantity_at_callback;
    };
    std::vector<Observed> seen;
    seen.reserve(4);
    Receiver receiver{7, [&](const Receiver& view, const Receiver::Notification& notification) {
        const auto* update = std::get_if<Message>(&notification);
        const auto quantity = update
            ? view.quantity_at(update->instrument_id, update->side, update->price_ticks)
            : std::nullopt;
        seen.push_back({notification, view.last_committed_sequence(), quantity});
    }};

    CHECK(seen.size() == 1);
    const auto* reset = std::get_if<Receiver::Reset>(&seen.front().notification);
    CHECK(reset && reset->session_id == 7 && reset->base_sequence == 0);
    CHECK(seen.front().committed == 0);

    auto send = [&](const Message& msg) {
        auto packet = encode(msg);
        CHECK(packet.has_value());
        const auto before = seen.size();
        receiver.on_datagram(*packet);
        CHECK(seen.size() == before + 1);
        const auto& observed = seen.back();
        const auto* update = std::get_if<Message>(&observed.notification);
        CHECK(update && encode(*update) == packet);
        CHECK(observed.committed == msg.seq_num);
        const auto expected_quantity = msg.operation == Operation::DELETE_LEVEL
            ? std::nullopt : std::optional{msg.quantity};
        CHECK(observed.quantity_at_callback == expected_quantity);
        CHECK(receiver.last_committed_sequence() == msg.seq_num);
    };

    send(msg1);
    CHECK(receiver.quantity_at(101, Side::BID, 100) == 10);

    auto check_after_rejection = [&] {
        CHECK(seen.size() == 2);  // only the reset and msg1 have been published
        CHECK(receiver.last_committed_sequence() == 1);
        CHECK(receiver.quantity_at(101, Side::BID, 100) == 10);
        CHECK(receiver.quantity_at(202, Side::ASK, 105) == std::nullopt);
    };

    // sequence 2 is due, but this datagram is one byte short.
    auto packet2 = encode(msg2);
    CHECK(packet2.has_value());
    receiver.on_datagram(std::span{*packet2}.first(packet2->size() - 1));
    check_after_rejection();

    // a complete, otherwise valid sequence 2 from a different session
    auto foreign = msg2;
    foreign.session_id = 8;
    auto foreign_packet = encode(foreign);
    CHECK(foreign_packet.has_value());
    receiver.on_datagram(*foreign_packet);
    check_after_rejection();

    send(msg2);
    CHECK(receiver.quantity_at(101, Side::BID, 100) == 10);
    CHECK(receiver.quantity_at(202, Side::ASK, 105) == 4);
    CHECK(receiver.quantity_at(101, Side::ASK, 105) == std::nullopt);
    send(msg3);
    CHECK(!receiver.quantity_at(101, Side::BID, 100).has_value());
    CHECK(receiver.quantity_at(202, Side::ASK, 105) == 4);
}
