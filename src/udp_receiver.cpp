#include "../include/udp_receiver.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace {
using Clock = std::chrono::steady_clock;

timespec to_timespec(Clock::duration wait) {
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(wait);
    const auto ns = std::chrono::ceil<std::chrono::nanoseconds>(wait - sec);
    return {static_cast<time_t>(sec.count()), static_cast<long>(ns.count())};
}

void read_ready(Receiver& receiver, pollfd socket, std::optional<Feed> feed,
                const UdpReadOptions* options) {
    if (socket.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw std::runtime_error("UDP receive socket reported an error");
    }
    if (!(socket.revents & POLLIN)) {
        return;
    }

    std::array<std::byte, wire_size + 1> bytes;  // retain evidence of an oversized datagram
    const auto n = ::recv(socket.fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "recv");
    }
    const auto packet = std::span{bytes}.first(static_cast<std::size_t>(n));
    if (options && options->on_read) options->on_read(feed, packet);
    if (feed) receiver.on_datagram(*feed, packet);
    else receiver.on_datagram(packet);
}

struct UdpBatch {
    static constexpr unsigned capacity = udp_batch_capacity;

    std::array<std::array<std::byte, wire_size + 1>, capacity> packets;
    std::array<iovec, capacity> buffers{};
    std::array<mmsghdr, capacity> messages{};

    UdpBatch() {
        // Each entry describes where the kernel should put one UDP datagram.
        for (std::size_t i = 0; i < capacity; ++i) {
            buffers[i] = {packets[i].data(), packets[i].size()};
            messages[i].msg_hdr.msg_iov = &buffers[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }
    }
    UdpBatch(const UdpBatch&) = delete;
    UdpBatch& operator=(const UdpBatch&) = delete;
};
void read_ready_batch(Receiver& receiver, pollfd socket, Feed feed,
                      UdpBatch& batch, const UdpReadOptions* options) {
    if (socket.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw std::runtime_error("UDP receive socket reported an error");
    }
    if (!(socket.revents & POLLIN)) {
        return;
    }

    const int count = ::recvmmsg(socket.fd, batch.messages.data(),
                                 UdpBatch::capacity, MSG_DONTWAIT, nullptr);
    if (count < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "recvmmsg");
    }

    if (count > 0 && options && options->batch_counts) {
        auto& bins = feed == Feed::A ? options->batch_counts->a : options->batch_counts->b;
        ++bins[static_cast<std::size_t>(count)];
    }

    // count is the number of datagrams. Each datagram has its own byte length.
    for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i) {
        const auto packet = std::span{batch.packets[i]}.first(batch.messages[i].msg_len);
        if (options && options->on_read) options->on_read(feed, packet);
        receiver.on_datagram(feed, packet);
    }
}

} // namespace

std::optional<ReplayRequest> receive_once(
    Receiver& receiver, int fd_a, int fd_b, Clock::duration gap_wait, int replay_fd, const UdpReadOptions* options) {
    if (fd_a < 0 || fd_b < 0 || fd_a == fd_b)
        throw std::invalid_argument("expected two distinct UDP feed descriptors");
    if (replay_fd < -1 || replay_fd == fd_a || replay_fd == fd_b)
        throw std::invalid_argument("expected a distinct UDP replay descriptor");
    std::array<pollfd, 3> fds{{{fd_a, POLLIN, 0}, {fd_b, POLLIN, 0}, {replay_fd, POLLIN, 0}}};
        
    UdpBatch batch{};
    for (;;) {
        const auto now = Clock::now();
        auto wait = receiver.retransmission_wait(now, gap_wait);
        if (options && options->wake_by) {
            const auto remaining = std::max(*options->wake_by - now, Clock::duration::zero());
            if (!wait || remaining < *wait) wait = remaining;
        }
        timespec timeout{};
        if (wait) timeout = to_timespec(*wait);
        const int ready = ::ppoll(fds.data(), fds.size(), wait ? &timeout : nullptr, nullptr);
        if (ready >= 0) break;
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "ppoll");
        // A signal interrupted the wait. Recompute the remaining time.
    }

    //read_ready(receiver, fds[0], Feed::A, options);
    //read_ready(receiver, fds[1], Feed::B, options);
    read_ready_batch(receiver, fds[0], Feed::A, batch, options);
    read_ready_batch(receiver, fds[1], Feed::B, batch, options);
    read_ready(receiver, fds[2], std::nullopt, options); // replay socket
    return receiver.poll_retransmission(Clock::now(), gap_wait);
}


