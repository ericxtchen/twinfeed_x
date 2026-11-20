#ifndef RECEIVER_H
#define RECEIVER_H

#include "order_book.h"
#include "protocol.h"
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <variant>
#include <array>

enum class Feed { A, B };

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
    };
    using Notification = std::variant<Reset, Message, Status>;
    // called synchornously and must not throw
    using Callback = std::function<void(const Receiver&, const Notification&)>;

    explicit Receiver(std::uint64_t active_session_id, Callback notify = {});
    void on_datagram(const std::span<const std::byte> &msg);
    // Both feed copies currently use the same sequencing path.
    void on_datagram(Feed, std::span<const std::byte> msg) { on_datagram(msg); }
    std::optional<std::uint64_t> quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const;
    [[nodiscard]] State state() const noexcept { return state_; }
    uint64_t last_committed_sequence() const {
        return last_committed_sequence_number_; 
    }
private:
    uint64_t active_session_id_;
    uint64_t last_committed_sequence_number_ = 0;
    std::unordered_map<std::uint32_t, OrderBook> books_;
    Callback notify_;
    State state_ = State::Running;
    // this holds Messages that are in limbo because messages arrived out of order
    std::array<Message, 64> buffer_;
    // this bitmap tells you if a certain index in buffer is in use right now
    // should be used to check if we can just overwrite or not
    uint64_t occupancy_bitmap_ = 0;
    void commit(const Message &msg) {
        books_[msg.instrument_id].apply(msg);
        last_committed_sequence_number_ = msg.seq_num;
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

