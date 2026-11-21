#ifndef UDP_RECEIVER_H
#define UDP_RECEIVER_H

#include "receiver.h"
#include <array>
#include <cstdint>
#include <functional>

inline constexpr unsigned udp_batch_capacity = 8;
using UdpBatchHistogram = std::array<std::uint64_t, udp_batch_capacity + 1>;

struct UdpBatchCounts {
    UdpBatchHistogram a{}, b{};
};

struct UdpReadOptions {
    // Wake a caller with a finite run even if the sockets stay quiet.
    std::optional<std::chrono::steady_clock::time_point> wake_by;
    // Runs immediately after recv. An empty leg identifies the replay socket.
    std::function<void(std::optional<Feed>, std::span<const std::byte>)> on_read;
    // Bin k counts live socket reads that returned k datagrams.
    UdpBatchCounts* batch_counts = nullptr;
};

// replay_fd is an optional UDP socket connected to the publisher's replay port.
std::optional<ReplayRequest> receive_once(Receiver& receiver, int fd_a, int fd_b,
    std::chrono::steady_clock::duration gap_wait, int replay_fd = -1, const UdpReadOptions* options = nullptr);

#endif
