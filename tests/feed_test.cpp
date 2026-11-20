#include "feed.h"
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const char* why) {
    if (!ok) throw std::runtime_error(why);
}

void replay_window() {
    using publisher::ReplayError;
    std::vector<WireMessage> a, b;
    publisher::Feed source{7, 4, [&](Feed leg, const WireMessage& packet) {
        (leg == Feed::A ? a : b).push_back(packet);
    }};
    Message update{};
    update.price_ticks = 100;
    update.instrument_id = 101;
    update.side = Side::BID;
    update.operation = Operation::UPSERT_LEVEL;
    // Session and sequence belong to the publisher, even if a caller supplied them.
    update.session_id = 99;
    update.seq_num = 99;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        update.quantity = i;
        source.publish(update).value();
        Message expected = update;
        expected.session_id = 7;
        expected.seq_num = i;
        check(a.size() == i && b.size() == i && a.back() == encode(expected).value() &&
              b.back() == a.back(), "expected one identical, correctly sequenced packet per leg");
    }
    check(source.replay({7, 1, 4}).value() == a, "full inclusive range should match live bytes");
    update.quantity = 0;
    check(source.publish(update) == std::unexpected(ProtocolError::UPSERT_WITH_ZERO_QUANTITY),
          "invalid publish should report the encoding error");
    check(a.size() == 4 && b.size() == 4 && source.replay({7, 1, 4}).value() == a,
          "invalid publish affected history or live delivery");

    update.quantity = 5;
    source.publish(update).value();
    check(source.replay({7, 1, 1}) == std::unexpected(ReplayError::Expired), "slot 1 was reused");
    check(source.replay({7, 1, 3}) == std::unexpected(ReplayError::Expired), "partly expired range");
    check(source.replay({7, 2, 5}).value() == std::vector<WireMessage>(a.begin() + 1, a.end()),
          "wrapped range must be in sequence order");
    check(source.replay({8, 2, 2}) == std::unexpected(ReplayError::WrongSession), "foreign session");
    check(source.replay({7, 0, 1}) == std::unexpected(ReplayError::InvalidRange), "zero sequence");
    check(source.replay({7, 4, 2}) == std::unexpected(ReplayError::InvalidRange), "reversed range");
    check(source.replay({7, 5, 6}) == std::unexpected(ReplayError::NotYetPublished), "future range");
}

void send_failure() {
    std::vector<WireMessage> b;
    publisher::Feed source{9, 1, [&](Feed leg, const WireMessage& packet) {
        if (leg == Feed::A) throw std::runtime_error("A send failed");
        b.push_back(packet);
    }};
    const Message update{0, 0, 100, 7, 101, Side::ASK, Operation::UPSERT_LEVEL};
    bool failed = false;
    try { source.publish(update).value(); }
    catch (const std::runtime_error&) { failed = true; }
    check(failed && b.size() == 1 && source.replay({9, 1, 1}).value() == b,
          "A send failure must still allow B delivery and replay");
}
} // namespace

int main() {
    try {
        replay_window();
        send_failure();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
