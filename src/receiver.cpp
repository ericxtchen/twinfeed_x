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
    auto check = decode(msg);
    if (!check) {
        return;
    }
    Message decoded = *check;
    if (decoded.session_id != this->active_session_id_) {
        return;
    }
    if (decoded.seq_num != this->last_committed_sequence_number_ + 1) {
        return;
    }
    this->books_[decoded.instrument_id].apply(decoded);
    this->last_committed_sequence_number_ = decoded.seq_num;
    if (notify_) {
        notify_(*this, decoded);
    }
}

std::optional<std::uint64_t> Receiver::quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const {
    if (auto it = this->books_.find(instrument_id); it != this->books_.end()) {
        return this->books_.at(instrument_id).quantity_at(side, ticks);
    }
    return std::nullopt;
}
