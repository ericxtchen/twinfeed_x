#include "../include/receiver.h"
#include <algorithm>
#include <chrono>
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
    if (decoded.seq_num <= last_committed_sequence_number_) {
        const Message& retained = recent_commits_[decoded.seq_num % recent_commits_.size()];
        if (retained.seq_num == decoded.seq_num && retained != decoded) {
            state_ = State::Error;
            notify_status(decoded.seq_num);
        }
        return;
    }

    const bool had_gap = known_gap();
    highest_observed_sequence_ = std::max(highest_observed_sequence_, decoded.seq_num);
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
            notify_status(decoded.seq_num);
            return;
        }
        buffer_[index] = decoded;
        occupancy_bitmap_ |= bit;
    } else {
        this->commit(decoded);
        this->drain();
    }
    if (state_ == State::Running && known_gap() != had_gap) {
        gap_first_polled_at_.reset();
        notify_status(decoded.seq_num);
    }
}

std::optional<std::chrono::steady_clock::duration> Receiver::retransmission_wait(
    std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration gap_wait) const {
    if (state_ != State::Running || !known_gap()) return std::nullopt;
    if (last_committed_sequence_number_ < replay_requested_through_) return std::nullopt;
    if (!gap_first_polled_at_) return std::chrono::steady_clock::duration::zero();
    const auto elapsed = now - *gap_first_polled_at_;
    if (elapsed >= gap_wait) return std::chrono::steady_clock::duration::zero();
    return gap_wait - elapsed;
}

std::optional<Receiver::ReplayRequest> Receiver::poll_retransmission(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration gap_wait) {
    if (state_ != State::Running || !known_gap()) return std::nullopt;
        const auto first_missing = last_committed_sequence_number_ + 1;
    if (first_missing <= replay_requested_through_) return std::nullopt;

    if (!gap_first_polled_at_) gap_first_polled_at_ = now;
    if (now - *gap_first_polled_at_ < gap_wait) return std::nullopt;

    // Request only the contiguous hole before the lowest buffered sequence.
    auto first_buffered = highest_observed_sequence_;
    for (std::size_t i = 0; i < buffer_.size(); ++i) {
        if (occupancy_bitmap_ & (1ULL << i))
            first_buffered = std::min(first_buffered, buffer_[i].seq_num);
    }
    replay_requested_through_ = first_buffered - 1;
    gap_first_polled_at_.reset();
    return ReplayRequest{active_session_id_, first_missing, replay_requested_through_};    
}

std::optional<std::chrono::steady_clock::time_point> Receiver::next_retransmission_deadline(std::chrono::steady_clock::duration gap_wait) const {
    if (state_ != State::Running || !known_gap() || !gap_first_polled_at_) {
        return std::nullopt;
    }
    return *gap_first_polled_at_ + gap_wait;
}

std::optional<std::uint64_t> Receiver::quantity_at(std::uint32_t instrument_id, Side side, std::int64_t ticks) const {
    if (auto it = this->books_.find(instrument_id); it != this->books_.end()) {
        return this->books_.at(instrument_id).quantity_at(side, ticks);
    }
    return std::nullopt;
}
