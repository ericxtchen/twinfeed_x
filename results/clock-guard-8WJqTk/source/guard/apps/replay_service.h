#pragma once

#include "../include/feed.h"
#include "loopback.h"
#include <iostream>

namespace publisher {

// The caller owns the wait loop. Each request can return several retained frames.
inline std::size_t reply_once(int fd, const Feed& source) {
    // One extra byte distinguishes an oversized control datagram from a valid one.
    std::array<std::byte, replay_request_wire_size + 1> bytes;
    sockaddr_in peer{};
    socklen_t size = sizeof(peer);
    const auto n = ::recvfrom(fd, bytes.data(), bytes.size(), 0,
                              reinterpret_cast<sockaddr*>(&peer), &size);
    if (n < 0) {
        if (errno == EINTR) return 0;
        throw std::system_error(errno, std::generic_category(), "recvfrom");
    }
    const auto request = decode_replay_request(std::span{bytes}.first(static_cast<std::size_t>(n)));
    if (!request) return 0;
    const auto packets = source.replay(*request);
    if (!packets) {
        std::cerr << "Replay unavailable for session " << request->session_id << " range "
                  << request->first_sequence << ".." << request->last_sequence << '\n';
        return 0;
    }
    for (const auto& packet : *packets) loopback::send_packet(fd, packet, &peer);
    return packets->size();
}

} // namespace publisher
