#include "../include/feed.h"
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace publisher {

Feed::Feed(std::uint64_t session_id, std::size_t capacity, Deliver deliver)
    : session_id_(session_id), history_(capacity), deliver_(std::move(deliver)) {
    if (capacity == 0) throw std::invalid_argument("feed replay capacity must be positive");
    if (!deliver_) throw std::invalid_argument("feed requires a live delivery callback");
}

std::expected<void, ProtocolError> Feed::publish(Message update) {
    if (last_sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("feed session exhausted its sequence numbers");
    update.session_id = session_id_;
    update.seq_num = last_sequence_ + 1;
    const auto packet = encode(update);
    if (!packet) return std::unexpected(packet.error());

    history_[update.seq_num % history_.size()] = {update.seq_num, *packet};
    last_sequence_ = update.seq_num;
    std::exception_ptr send_error;
    for (const auto leg : {::Feed::A, ::Feed::B}) {
        try {
            deliver_(leg, *packet);
        } catch (...) {
            if (!send_error) send_error = std::current_exception();
        }
    }
    if (send_error) std::rethrow_exception(send_error);
    return {};
}

std::expected<std::vector<WireMessage>, ReplayError> Feed::replay(const ReplayRequest& request) const {
    const auto [session, first, last] = request;
    if (session != session_id_) return std::unexpected(ReplayError::WrongSession);
    if (first == 0 || first > last) return std::unexpected(ReplayError::InvalidRange);
    if (last > last_sequence_) return std::unexpected(ReplayError::NotYetPublished);
    if (history_[first % history_.size()].sequence != first) return std::unexpected(ReplayError::Expired);

    // Successful publishes are contiguous. If the first is retained, the whole range is present.
    const auto count = last - first + 1;
    std::vector<WireMessage> packets;
    packets.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t offset = 0; offset < count; ++offset)
        packets.push_back(history_[(first + offset) % history_.size()].packet);
    return packets;
}

} // namespace publisher
