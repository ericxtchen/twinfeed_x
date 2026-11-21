#include "receiver.h"
#include "feed.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Seq = std::uint64_t;

void require(bool ok, std::string_view why) {
    if (!ok) throw std::runtime_error(std::string(why));
}

// One price level gives the trace an independent, inspectable book at every sequence.
Message event(Seq seq) {
    const bool erase = seq % 5 == 0;
    return {.session_id = 7, .seq_num = seq, .price_ticks = 100, .quantity = erase ? 0 : seq,
            .instrument_id = 101, .side = Side::BID,
            .operation = erase ? Operation::DELETE_LEVEL : Operation::UPSERT_LEVEL};
}

std::optional<Seq> expected_quantity(Seq seq) {
    if (seq % 5 == 0) return std::nullopt;
    return seq;
}

// Let the rest of the suite run while the public gap query is still being implemented.
std::optional<bool> gap_at(const auto& receiver) {
    if constexpr (requires { receiver.known_gap(); }) return receiver.known_gap();
    else return std::nullopt;
}

struct Update {
    Message message;
    Seq committed;
    std::optional<Seq> quantity;
};

struct Status {
    Receiver::State state;
    Seq incoming;
    Seq committed;
    std::optional<bool> gap;
    bool operator==(const Status&) const = default;
};

struct Replay {
    std::vector<Receiver::Reset> resets;
    std::vector<Update> updates;
    std::vector<Status> statuses;
    Receiver receiver{7, [this](const Receiver& view, const Receiver::Notification& notice) {
        if (auto* update = std::get_if<Message>(&notice)) {
            updates.push_back({*update, view.last_committed_sequence(), view.quantity_at(101, Side::BID, 100)});
        } else if (auto* status = std::get_if<Receiver::Status>(&notice)) {
            statuses.push_back({status->state, status->incoming_sequence, view.last_committed_sequence(), gap_at(view)});
        } else {
            resets.push_back(std::get<Receiver::Reset>(notice));
        }
    }};

    void send(Feed feed, Seq seq) { receiver.on_datagram(feed, encode(event(seq)).value()); }

    void expect_prefix(Seq end) const {
        require(resets.size() == 1 && resets[0].session_id == 7 && resets[0].base_sequence == 0,
                "expected only the bootstrap reset at 0");
        require(receiver.last_committed_sequence() == end && updates.size() == end, "wrong commit or update count");
        require(receiver.quantity_at(101, Side::BID, 100) == expected_quantity(end), "wrong live book");
        for (Seq seq = 1; seq <= end; ++seq) {
            const auto& actual = updates[seq - 1];
            if (actual.message != event(seq) || actual.committed != seq || actual.quantity != expected_quantity(seq))
                throw std::runtime_error("incorrect update or callback book at sequence " + std::to_string(seq));
        }
    }

    void expect_terminal(Receiver::State expected, Seq incoming, Seq committed) const {
        if (receiver.state() != expected) {
            const char* name = expected == Receiver::State::Error ? "Error" : "NeedsRecovery";
            throw std::runtime_error("expected " + std::string(name) + " on arrival " + std::to_string(incoming) +
                                     " with committed sequence " + std::to_string(committed));
        }
        require(!statuses.empty() && statuses.back().state == expected && statuses.back().incoming == incoming &&
                statuses.back().committed == committed, "missing or incorrect terminal status");
    }
};

void fill_ring(Replay& replay) {
    replay.send(Feed::A, 1);
    for (Seq seq = 3; seq <= 66; ++seq) replay.send(Feed::A, seq);
    replay.expect_prefix(1);
    require(replay.receiver.state() == Receiver::State::Running, "filling the last slot stopped the receiver");
}

void full_buffer_gap_closure() {
    Replay replay;
    fill_ring(replay);
    const auto statuses = replay.statuses.size();
    for (int i = 0; i < 128; ++i) {
        replay.send(Feed::B, 3);  // Buffered copy.
        replay.send(Feed::B, 1);  // Already committed.
    }
    replay.expect_prefix(1);
    require(replay.statuses.size() == statuses, "duplicate burst published a status");
    replay.send(Feed::B, 2);      // Slot 2 holds 66; the predecessor needs no new slot.
    replay.expect_prefix(66);
    require(replay.receiver.state() == Receiver::State::Running, "gap closure entered a terminal state");
    replay.send(Feed::A, 68);     // Reuse a slot that was occupied before the drain.
    replay.send(Feed::B, 67);
    replay.expect_prefix(68);
}

void full_buffer_overflow() {
    Replay replay;
    fill_ring(replay);
    const auto before = replay.statuses.size();
    replay.send(Feed::B, 67);
    replay.expect_terminal(Receiver::State::NeedsRecovery, 67, 1);
    replay.expect_prefix(1);
    replay.send(Feed::A, 67);
    replay.send(Feed::B, 2);
    replay.expect_terminal(Receiver::State::NeedsRecovery, 67, 1);
    replay.expect_prefix(1);
    require(replay.statuses.size() == before + 1, "expected exactly one recovery transition");
}

void known_gap() {
    Replay replay;
    auto expect_gap = [&](bool expected) {
        const auto gap = gap_at(replay.receiver);
        require(gap.has_value(), "Receiver needs a public known_gap() query");
        require(*gap == expected, "incorrect known_gap()");
    };
    expect_gap(false);
    replay.send(Feed::A, 1);
    auto foreign = event(90);
    ++foreign.session_id;
    replay.receiver.on_datagram(Feed::B, encode(foreign).value());
    const auto truncated = encode(event(91)).value();
    replay.receiver.on_datagram(Feed::A, std::span{truncated}.first(wire_size - 1));
    expect_gap(false);
    const auto before = replay.statuses.size();
    replay.send(Feed::A, 3);
    expect_gap(true);
    require(replay.statuses.size() == before + 1 && replay.statuses.back().gap == true &&
            replay.statuses.back().committed == 1, "missing gap-open status at sequence 1");
    replay.send(Feed::B, 3);
    replay.send(Feed::A, 5);  // Neither feed supplies 4 during this part of the trace.
    replay.send(Feed::B, 2);
    replay.expect_prefix(3);
    expect_gap(true);
    const auto before_close = replay.statuses.size();
    replay.send(Feed::B, 4);
    replay.expect_prefix(5);
    expect_gap(false);
    require(replay.statuses.size() == before_close + 1 && replay.statuses.back().gap == false &&
            replay.statuses.back().committed == 5, "missing gap-closed status at sequence 5");
}

// Deliver both live paths except for sequence 2. The publisher still retains it.
publisher::Feed source_missing_live_two(Replay& replay) {
    return {7, 4, [&replay](Feed leg, const WireMessage& packet) {
        if (decode(packet).value().seq_num != 2) replay.receiver.on_datagram(leg, packet);
    }};
}

void publisher_replay() {
    Replay replay;
    auto source = source_missing_live_two(replay);
    for (Seq seq = 1; seq <= 3; ++seq) source.publish(event(seq)).value();
    replay.expect_prefix(1);
    require(replay.receiver.known_gap(), "live loss of sequence 2 should open a gap");
    const auto packets = source.replay({7, 2, 2}).value();
    require(packets.size() == 1 && packets.front() == encode(event(2)).value(), "wrong replay response");
    replay.receiver.on_datagram(packets.front());
    replay.expect_prefix(3);
    require(!replay.receiver.known_gap(), "publisher replay should close the gap");
}

void retransmission_deadline() {
    using namespace std::chrono_literals;
    constexpr auto gap_wait = 10us;
    std::chrono::steady_clock::time_point now{};
    Replay replay;
    auto source = source_missing_live_two(replay);
    auto poll = [&] { return replay.receiver.poll_retransmission(now, gap_wait); };

    source.publish(event(1)).value();
    require(!poll(), "unexpected replay request before a gap");
    now += 100us;
    source.publish(event(2)).value();  // Retained by the publisher, dropped on both paths.
    source.publish(event(3)).value();
    replay.expect_prefix(1);
    require(replay.receiver.known_gap(), "expected a gap at sequence 2");
    require(!poll(), "requested replay as soon as the gap opened");

    now += gap_wait - 1us;
    require(!poll(), "requested replay before the gap deadline");
    now += 1us;
    const auto request = poll();
    require(request.has_value(), "gap deadline: expected one replay request for session 7, range [2, 2]");
    require(request->session_id == 7 && request->first_sequence == 2 && request->last_sequence == 2,
            "incorrect replay session or inclusive range");
    require(!poll(), "polling twice at the deadline emitted a duplicate request");
    replay.expect_prefix(1);
    require(replay.receiver.known_gap(), "issuing the request cleared the gap");

    // The replay source supplies the requested wire message to the same sequencer as A/B.
    const auto response = source.replay(*request).value();
    for (const auto& packet : response) replay.receiver.on_datagram(packet);
    replay.expect_prefix(3);  // Includes update order, the book at each callback, and just the bootstrap reset.
    require(!replay.receiver.known_gap() && replay.receiver.state() == Receiver::State::Running,
            "replay of 2 should drain 3 and close the gap");
    require(!poll(), "requested replay after the gap closed");

    const auto status_count = replay.statuses.size();
    replay.send(Feed::B, 2);
    replay.expect_prefix(3);
    require(replay.statuses.size() == status_count, "late live copy emitted a status");
    now += gap_wait;
    require(!poll(), "requested sequence 2 again after recovery");
}

enum class Copy { LiveB, ReplayReply };

void replay_race(Copy first) {
    using namespace std::chrono_literals;
    Replay trace;
    std::optional<WireMessage> delayed_b;
    publisher::Feed source{7, 8, [&](Feed leg, const WireMessage& packet) {
        if (decode(packet).value().seq_num == 4) {
            if (leg == Feed::B) delayed_b = packet;
            return;
        }
        trace.receiver.on_datagram(leg, packet);
    }};
    for (Seq seq = 1; seq <= 5; ++seq) source.publish(event(seq)).value();

    const std::chrono::steady_clock::time_point start{};
    require(!trace.receiver.poll_retransmission(start, 10us), "requested before gap wait");
    const auto request = trace.receiver.poll_retransmission(start + 10us, 10us);
    require(request && request->session_id == 7 && request->first_sequence == 4 &&
            request->last_sequence == 4, "expected request for session 7, range [4,4]");
    trace.expect_prefix(3);
    require(trace.receiver.known_gap(), "outstanding request should leave the gap open");
    const auto reply = source.replay(*request).value();
    require(delayed_b && reply.size() == 1 && *delayed_b == reply.front(), "expected two copies of 4");

    auto deliver = [&](Copy copy) {
        if (copy == Copy::LiveB) trace.receiver.on_datagram(Feed::B, *delayed_b);
        else trace.receiver.on_datagram(reply.front());
    };
    const std::vector<Status> expected_statuses{
        {Receiver::State::Running, 5, 3, true},
        {Receiver::State::Running, 4, 5, false}
    };
    const auto second = first == Copy::LiveB ? Copy::ReplayReply : Copy::LiveB;
    for (const auto copy : {first, second}) {
        deliver(copy);
        trace.expect_prefix(5);
        require(trace.receiver.state() == Receiver::State::Running && !trace.receiver.known_gap(),
                "receiver should be running with the gap closed");
        require(trace.statuses == expected_statuses, "expected one gap-open and one gap-closed status");
    }
}

void committed_conflict(std::initializer_list<Seq> arrivals, Seq disputed) {
    Replay replay;
    for (const Seq seq : arrivals) replay.send(Feed::A, seq);
    const Seq last = *std::max_element(arrivals.begin(), arrivals.end());
    replay.expect_prefix(last);
    const auto before = replay.statuses.size();
    replay.send(Feed::B, disputed);
    require(replay.statuses.size() == before, "identical late copy emitted a status");
    auto other = event(disputed);
    ++other.quantity;
    replay.receiver.on_datagram(Feed::B, encode(other).value());
    replay.expect_terminal(Receiver::State::Error, disputed, last);
    replay.expect_prefix(last);
    replay.send(Feed::A, last + 1);
    replay.expect_terminal(Receiver::State::Error, disputed, last);
    replay.expect_prefix(last);
    require(replay.statuses.size() == before + 1, "expected exactly one consistency error");
}

void restart() {
    {
        Replay first;
        first.send(Feed::A, 1);
        first.send(Feed::A, 3);
        first.send(Feed::B, 67);
        first.expect_terminal(Receiver::State::NeedsRecovery, 67, 1);
    }
    Replay fresh;
    fresh.expect_prefix(0);
    fresh.send(Feed::B, 2);
    fresh.expect_prefix(0);
    fresh.send(Feed::A, 1);
    fresh.expect_prefix(2);  // The old run's buffered 3 must be gone.
}

struct Arrival { Feed feed; Seq seq; };

void seeded_replay() {
    constexpr Seq length = 32;  // All futures fit; a slot collision cannot explain a missing update.
    for (unsigned seed = 0; seed < 64; ++seed) {
        std::mt19937 rng(seed);
        const Seq missing = seed % 2 == 0 ? 1 + rng() % (length - 1) : 0;
        std::vector<Arrival> trace;
        for (Seq seq = 1; seq <= length; ++seq) {
            if (seq == missing) continue;
            const auto copies = 1U + rng() % 3U;
            for (unsigned i = 0; i < copies; ++i)
                trace.push_back({(rng() & 1U) ? Feed::A : Feed::B, seq});
        }
        std::shuffle(trace.begin(), trace.end(), rng);
        try {
            auto run = [&] {
                Replay replay;
                std::array<bool, length + 1> arrived{};
                Seq prefix = 0;
                for (const auto& arrival : trace) {
                    arrived[arrival.seq] = true;
                    while (prefix < length && arrived[prefix + 1]) ++prefix;
                    replay.send(arrival.feed, arrival.seq);
                    replay.expect_prefix(prefix);
                    require(replay.receiver.state() == Receiver::State::Running, "unexpected terminal state");
                }
                return replay.statuses;
            };
            const auto first = run();
            require(run() == first, "status history changed when replaying identical arrivals");
        } catch (const std::exception&) {
            std::cerr << "seed " << seed << ", session 7, arrivals";
            for (const auto& a : trace) std::cerr << ' ' << (a.feed == Feed::A ? 'A' : 'B') << a.seq;
            std::cerr << '\n';
            throw;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::string_view name = argv[1];
        if (name == "full_buffer_gap_closure") full_buffer_gap_closure();
        else if (name == "full_buffer_overflow") full_buffer_overflow();
        else if (name == "known_gap") known_gap();
        else if (name == "retransmission_deadline") retransmission_deadline();
        else if (name == "live_before_replay") replay_race(Copy::LiveB);
        else if (name == "replay_before_live") replay_race(Copy::ReplayReply);
        else if (name == "publisher_replay") publisher_replay();
        else if (name == "committed_conflict") committed_conflict({1}, 1);
        else if (name == "drained_conflict") committed_conflict({2, 1}, 2);
        else if (name == "recent_conflict") committed_conflict({1, 2}, 1);
        else if (name == "restart") restart();
        else if (name == "seeded_replay") seeded_replay();
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
