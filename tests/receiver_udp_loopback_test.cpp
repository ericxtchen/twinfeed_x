#include "protocol.h"
#include "feed.h"
#include "receiver.h"
#ifdef __linux__
#include "udp_receiver.h"
#endif

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
#include <string_view>
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

#ifndef __linux__
// The portable loopback test also runs on systems without Linux ppoll().
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
#endif

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
#ifdef __linux__
        if (receive_once(receiver, a.fd, b.fd, std::chrono::hours{1}))
            throw std::runtime_error("unexpected replay request");
#else
        if (receive_one(a, b, receiver) != feed) throw std::runtime_error("incorrect feed");
#endif
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

    const auto p1 = encode(first).value();
    std::array<std::byte, wire_size + 8> oversized{};
    std::copy(p1.begin(), p1.end(), oversized.begin());
    deliver(Feed::A, oversized);
    if (receiver.last_committed_sequence() != 0 || !updates.empty())
        throw std::runtime_error("oversized datagram was accepted as a valid prefix");

    std::optional<WireMessage> delayed_b2;
    publisher::Feed source{7, 4, [&](Feed leg, const WireMessage& packet) {
        const auto seq = decode(packet).value().seq_num;
        if (seq == 2) {
            if (leg == Feed::B) delayed_b2 = packet;
            return;
        }
        if (seq == 1 && leg == Feed::B) return;
        deliver(leg, packet);
    }};
    source.publish(first).value();
    source.publish(second).value();
    source.publish(third).value();
    if (updates != std::vector<Observation>{{1, 10}})
        throw std::runtime_error("published across a gap or duplicated an update");
    deliver(Feed::B, delayed_b2.value());
    if (updates != std::vector<Observation>{{1, 10}, {2, 20}, {3, std::nullopt}} ||
        receiver.last_committed_sequence() != 3 || receiver.quantity_at(101, Side::BID, 100))
        throw std::runtime_error("incorrect loopback commit history or final book");
}
#ifdef __linux__
void backlog_and_deadline() {
    Socket a, b, sender;
    const auto dest_a = bind_loopback(a);
    const auto dest_b = bind_loopback(b);
    std::vector<std::uint64_t> updates;
    unsigned resets = 0;
    Receiver receiver{7, [&](const Receiver&, const Receiver::Notification& notice) {
        if (const auto* msg = std::get_if<Message>(&notice)) updates.push_back(msg->seq_num);
        if (std::holds_alternative<Receiver::Reset>(notice)) ++resets;
    }};
    auto step = [&] { return receive_once(receiver, a.fd, b.fd, std::chrono::milliseconds{5}); };
    publisher::Feed source{7, 8, [&](Feed leg, const WireMessage& packet) {
        const auto seq = decode(packet).value().seq_num;
        if (seq == 4) return;  // Both live copies are lost.
        if (leg == (seq == 2 ? Feed::B : Feed::A))
            publish(sender, leg == Feed::A ? dest_a : dest_b, packet);
    }};
    const Message update{0, 0, 100, 10, 101, Side::BID, Operation::UPSERT_LEVEL};
    for (int i = 0; i < 3; ++i) source.publish(update).value();
    // A has 1 and 3 queued. B already has 2. Serve one from each ready socket.
    if (step() || receiver.last_committed_sequence() != 2 || receiver.known_gap())
        throw std::runtime_error("first turn should serve A1 and B2");
    if (step() || receiver.last_committed_sequence() != 3)
        throw std::runtime_error("second turn should consume queued A3");

    source.publish(update).value();  // 4 retained for replay only
    source.publish(update).value();  // 5 reveals the gap
    if (step() || !receiver.known_gap()) throw std::runtime_error("expected a gap at 4");
    const auto request = step();    // No further UDP input. Only the deadline can wake this call.
    if (!request || request->session_id != 7 || request->first_sequence != 4 ||
        request->last_sequence != 4 || receiver.last_committed_sequence() != 3)
        throw std::runtime_error("timeout should request [4,4] without advancing the book");
    const auto response = source.replay(*request).value();
    for (const auto& packet : response) receiver.on_datagram(packet);
    if (updates != std::vector<std::uint64_t>{1, 2, 3, 4, 5} || resets != 1 || receiver.known_gap())
        throw std::runtime_error("replay should commit 4 and drain 5");
}
#endif
} // namespace

int main(int argc, [[maybe_unused]] char** argv) {
    try {
        if (argc == 1) loopback();
#ifdef __linux__
        else if (argc == 2 && std::string_view(argv[1]) == "deadline") backlog_and_deadline();
#endif
        else return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
