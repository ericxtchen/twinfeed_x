#ifndef RECEIVER_H
#define RECEIVER_H

#include "order_book.h"
#include "protocol.h"
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <variant>

class Receiver {
public:
    // Reset struct indicates a start of a session or reset
    struct Reset {
        std::uint64_t session_id;
        std::uint64_t base_sequence;
    };
    // a consumer listening to twinfeed_x gets these Notifications
    // either a reset or a Message that is like an update
    using Notification = std::variant<Reset, Message>;
    // called synchornously and must not throw
    using Callback = std::function<void(const Receiver&, const Notification&)>;

    explicit Receiver(std::uint64_t active_session_id, Callback notify = {});
    void on_datagram(const std::span<const std::byte> &msg);
    std::optional<std::uint64_t> quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const;
    uint64_t last_committed_sequence() const {
        return last_committed_sequence_number_; 
    }
private:
    uint64_t active_session_id_;
    uint64_t last_committed_sequence_number_ = 0;
    std::unordered_map<std::uint32_t, OrderBook> books_;
    Callback notify_;
};

#endif

