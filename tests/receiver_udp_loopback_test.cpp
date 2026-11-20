#include "protocol.h"
#include "receiver.h"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
void check_sys(int result, const char* operation) {
    if (result < 0) throw std::system_error(errno, std::generic_category(), operation);
}

struct Socket {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    Socket() { check_sys(fd, "socket"); }
    ~Socket() { ::close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};

sockaddr_in bind_loopback(const Socket& socket) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check_sys(::bind(socket.fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), "bind");
    socklen_t len = sizeof(addr);
    check_sys(::getsockname(socket.fd, reinterpret_cast<sockaddr*>(&addr), &len), "getsockname");
    return addr;  // The kernel chose the port.
}

void publish(const Socket& sender, const sockaddr_in& dest, std::span<const std::byte> packet) {
    const auto sent = ::sendto(sender.fd, packet.data(), packet.size(), 0,
                               reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) throw std::system_error(errno, std::generic_category(), "sendto");
    if (static_cast<std::size_t>(sent) != packet.size()) throw std::runtime_error("incomplete datagram");
}

// Test driver for the existing byte-ingest API. Replace this call with the shared
// receive step when the UDP application is added.
Feed receive_one(const Socket& a, const Socket& b, Receiver& receiver) {
    std::array<pollfd, 2> fds{{{a.fd, POLLIN, 0}, {b.fd, POLLIN, 0}}};
    const int ready = ::poll(fds.data(), fds.size(), 1000);
    check_sys(ready, "poll");
    for (const auto& fd : fds) {
        if (!(fd.revents & POLLIN)) continue;
        std::array<std::byte, wire_size + 1> bytes{}; // Preserve an oversize indication for decode().
        const auto count = ::recv(fd.fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (count < 0) throw std::system_error(errno, std::generic_category(), "recv");
        const Feed feed = fd.fd == a.fd ? Feed::A : Feed::B;
        receiver.on_datagram(feed, std::span{bytes}.first(static_cast<std::size_t>(count)));
        return feed;
    }
    throw std::runtime_error("no loopback datagram readable before deadline");
}

void loopback() {
    Socket a, b, sender;
    const auto dest_a = bind_loopback(a);
    const auto dest_b = bind_loopback(b);
    using Observation = std::pair<std::uint64_t, std::optional<std::uint64_t>>;
    std::vector<Observation> updates;
    Receiver receiver{7, [&](const Receiver& view, const Receiver::Notification& notice) {
        if (const auto* msg = std::get_if<Message>(&notice)) {
            updates.emplace_back(view.last_committed_sequence(),
                                 view.quantity_at(msg->instrument_id, msg->side, msg->price_ticks));
        }
    }};
    auto deliver = [&](Feed feed, std::span<const std::byte> packet) {
        publish(sender, feed == Feed::A ? dest_a : dest_b, packet);
        // Process each arrival before sending the next. Two sockets have no shared arrival order.
        if (receive_one(a, b, receiver) != feed) throw std::runtime_error("incorrect feed");
    };
    const Message first{.session_id = 7, .seq_num = 1, .price_ticks = 100, .quantity = 10,
                        .instrument_id = 101, .side = Side::BID, .operation = Operation::UPSERT_LEVEL};
    auto second = first;
    second.seq_num = 2;
    second.quantity = 20;
    auto third = first;
    third.seq_num = 3;
    third.quantity = 0;
    third.operation = Operation::DELETE_LEVEL;

    const auto p1 = encode(first).value(), p2 = encode(second).value(), p3 = encode(third).value();
    std::array<std::byte, wire_size + 8> oversized{};
    std::copy(p1.begin(), p1.end(), oversized.begin());
    deliver(Feed::A, oversized);
    if (receiver.last_committed_sequence() != 0 || !updates.empty())
        throw std::runtime_error("oversized datagram was accepted as a valid prefix");

    deliver(Feed::A, p1);
    deliver(Feed::A, p3);
    deliver(Feed::B, p3);
    if (updates != std::vector<Observation>{{1, 10}})
        throw std::runtime_error("published across a gap or duplicated an update");
    deliver(Feed::B, p2);
    if (updates != std::vector<Observation>{{1, 10}, {2, 20}, {3, std::nullopt}} ||
        receiver.last_committed_sequence() != 3 || receiver.quantity_at(101, Side::BID, 100))
        throw std::runtime_error("incorrect loopback commit history or final book");
}
} // namespace

int main() {
    try {
        loopback();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
