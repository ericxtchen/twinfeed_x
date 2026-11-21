#pragma once

#include "protocol.h"
#include <functional>
#include <vector>

namespace publisher {

enum class ReplayError { WrongSession, InvalidRange, NotYetPublished, Expired };

// Single-writer publisher for one session. The caller supplies the live transport.
class Feed {
public:
    using Deliver = std::function<void(::Feed, const WireMessage&)>;

    Feed(std::uint64_t session_id, std::size_t capacity, Deliver deliver);
    Feed(const Feed&) = delete;
    Feed& operator=(const Feed&) = delete;

    // Assigns session_id and seq_num. Invalid payloads consume no sequence number.
    // A delivery exception is propagated after both live paths have been attempted.
    [[nodiscard]] std::expected<void, ProtocolError> publish(Message update);
    [[nodiscard]] std::expected<std::vector<WireMessage>, ReplayError> replay(const ReplayRequest& request) const;

private:
    struct Entry {
        std::uint64_t sequence = 0;
        WireMessage packet{};
    };
    std::uint64_t session_id_;
    std::uint64_t last_sequence_ = 0;
    std::vector<Entry> history_;
    Deliver deliver_;
};

} // namespace publisher
