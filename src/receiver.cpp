#include "../include/receiver.h"
#include <cstdint>
#include <optional>
#include <utility>

Receiver::Receiver(std::uint64_t active_session_id, Callback notify) 
: active_session_id_(active_session_id), notify_(std::move(notify)) 
{
    if (notify_) {
        notify_(*this, Reset{active_session_id_, 0});    
    }
}

void Receiver::on_datagram(const std::span<const std::byte> &msg) {
    if (state_ != State::Running) return;

    auto check = decode(msg);
    if (!check) {
        return;
    }
    Message decoded = *check;
    if (decoded.session_id != this->active_session_id_) {
        return;
    }
    if (decoded.seq_num <= last_committed_sequence_number_) return;
    if (decoded.seq_num - last_committed_sequence_number_ > 1) {
        const auto index = decoded.seq_num & 63;
        const auto bit = 1ULL << index;
        if (occupancy_bitmap_ & bit) {
            const Message& held = buffer_[index];
            if (held.seq_num != decoded.seq_num) {
                state_ = State::NeedsRecovery;   // Different sequences need the same slot.
            } else if (held != decoded) {
                state_ = State::Error;           // Contradictory copies of one sequence.
            } else {
                return;                         // Identical buffered copy.
            }
            if (notify_) notify_(*this, Status{state_, decoded.seq_num});
            return;
        }
        buffer_[index] = decoded;
        occupancy_bitmap_ |= bit;
        return;
    }
    this->commit(decoded);
    this->drain();
}

std::optional<std::uint64_t> Receiver::quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const {
    if (auto it = this->books_.find(instrument_id); it != this->books_.end()) {
        return this->books_.at(instrument_id).quantity_at(side, ticks);
    }
    return std::nullopt;
}
