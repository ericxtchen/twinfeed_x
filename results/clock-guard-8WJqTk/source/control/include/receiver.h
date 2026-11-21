#ifndef RECEIVER_H
#define RECEIVER_H

#include "order_book.h"
#include "protocol.h"
#include <chrono>
#include <optional>
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <variant>
#include <array>

class Receiver {
public:
    // Reset struct indicates a start of a session or reset
    struct Reset {
        std::uint64_t session_id;
        std::uint64_t base_sequence;
    };
    // Running accepts packets, including while waiting for a missing predecessor.
    enum class State { Running, NeedsRecovery, Error };
    struct Status {
        State state;
        std::uint64_t incoming_sequence;
        bool known_gap = false;
    };
    using Notification = std::variant<Reset, Message, Status>;
    // called synchornously and must not throw
    using Callback = std::function<void(const Receiver&, const Notification&)>;

    using ReplayRequest = ::ReplayRequest;
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration> retransmission_wait(
        std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration gap_wait) const;
    // Poll after processing an arrival and on timer ticks, using the same monotonic clock.
    // this will later be refactored into a more event/interrupt driven over another thread
    [[nodiscard]] std::optional<ReplayRequest> poll_retransmission(
        std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration gap_wait);

    explicit Receiver(std::uint64_t active_session_id, Callback notify = {});
    void on_datagram(const std::span<const std::byte> &msg);
    // Both feed copies currently use the same sequencing path.
    void on_datagram(Feed, std::span<const std::byte> msg) { on_datagram(msg); }
    std::optional<std::uint64_t> quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const;
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool known_gap() const noexcept {
        return last_committed_sequence_number_ < highest_observed_sequence_;
    }
    uint64_t last_committed_sequence() const {
        return last_committed_sequence_number_; 
    }
    std::optional<std::chrono::steady_clock::time_point> next_retransmission_deadline(std::chrono::steady_clock::duration gap_wait) const;
private:
    uint64_t active_session_id_;
    uint64_t last_committed_sequence_number_ = 0;
    std::uint64_t highest_observed_sequence_ = 0;
    // gap_first_polled_at_ represents the first time we called poll_retransmission
    // this is changed against now and gap_wait to see if we have to ask for retransmission from a feed
    std::optional<std::chrono::steady_clock::time_point> gap_first_polled_at_;
    std::uint64_t replay_requested_through_ = 0;
    // A full sequence number distinguishes a retained copy from an overwritten slot.
    // this saves our most recent 64 commits
    // this allows us to buy 64 commits worth of detection coverage
    // the size can be toggled from 0 where you have no detection coverage and you unconditionally accept the first valid commit of a sequence number
    // to however big you want
    std::array<Message, 64> recent_commits_{};
    std::unordered_map<std::uint32_t, OrderBook> books_;
    Callback notify_;
    State state_ = State::Running;
    // this holds Messages that are in limbo because messages arrived out of order
    std::array<Message, 64> buffer_;
    // this bitmap tells you if a certain index in buffer is in use right now
    // should be used to check if we can just overwrite or not
    uint64_t occupancy_bitmap_ = 0;
    void notify_status(std::uint64_t incoming_sequence) {
        if (notify_) notify_(*this, Status{state_, incoming_sequence, known_gap()});
    }
    void commit(const Message &msg) {
        books_[msg.instrument_id].apply(msg);
        last_committed_sequence_number_ = msg.seq_num;
        recent_commits_[msg.seq_num % recent_commits_.size()] = msg;
        if (notify_) {
            notify_(*this, msg);
        }
    }
    void drain() {
        while (state_ == State::Running) {
            uint64_t next = last_committed_sequence_number_ + 1;
            uint64_t index = next & 63;
            uint64_t bit = 1ULL << index;
            if (!(occupancy_bitmap_ & bit)) {
                break;
            }
            if (buffer_[index].seq_num != next) {
                return;
            }
            Message msg = std::move(buffer_[index]);
            occupancy_bitmap_ &= ~bit;
            this->commit(msg);
        }
    }
};

#endif

