#include "../include/udp_receiver.h"

#include <poll.h>
#include <sys/socket.h>
#include <time.h>

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

void read_ready(Receiver& receiver, pollfd socket, std::optional<Feed> feed) {
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
    if (feed) receiver.on_datagram(*feed, packet);
    else receiver.on_datagram(packet);
}
} // namespace

std::optional<ReplayRequest> receive_once(
    Receiver& receiver, int fd_a, int fd_b, Clock::duration gap_wait, int replay_fd) {
    if (fd_a < 0 || fd_b < 0 || fd_a == fd_b)
        throw std::invalid_argument("expected two distinct UDP feed descriptors");
    if (replay_fd < -1 || replay_fd == fd_a || replay_fd == fd_b)
        throw std::invalid_argument("expected a distinct UDP replay descriptor");
    std::array<pollfd, 3> fds{{{fd_a, POLLIN, 0}, {fd_b, POLLIN, 0}, {replay_fd, POLLIN, 0}}};

    for (;;) {
        const auto wait = receiver.retransmission_wait(Clock::now(), gap_wait);
        timespec timeout{};
        if (wait) timeout = to_timespec(*wait);
        const int ready = ::ppoll(fds.data(), fds.size(), wait ? &timeout : nullptr, nullptr);
        if (ready >= 0) break;
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "ppoll");
        // A signal interrupted the wait. Recompute the remaining time.
    }

    read_ready(receiver, fds[0], Feed::A);
    read_ready(receiver, fds[1], Feed::B);
    read_ready(receiver, fds[2], std::nullopt);
    return receiver.poll_retransmission(Clock::now(), gap_wait);
}


