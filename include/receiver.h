#ifndef RECEIVER_H
#define RECEIVER_H

#include "order_book.h"
#include <unordered_map>
#include <cstdint>

class Receiver {
public:
    explicit Receiver(std::uint64_t active_session_id) : active_session_id_(active_session_id) {}
    void on_datagram(const std::span<const std::byte> &msg);
    std::optional<std::uint64_t> quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const;
    uint64_t last_committed_sequence() const {
        return last_committed_sequence_number_; 
    }
private:
    uint64_t active_session_id_ = 0;
    uint64_t last_committed_sequence_number_ = 0;
    std::unordered_map<std::uint32_t, OrderBook> books_;
};

#endif

