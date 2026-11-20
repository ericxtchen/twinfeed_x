#include "loopback.h"
#include "../include/udp_receiver.h"

#include <iostream>

namespace {
void report(const Receiver&, const Receiver::Notification& notification) {
    if (const auto* reset = std::get_if<Receiver::Reset>(&notification))
        std::cout << "reset " << reset->session_id << ' ' << reset->base_sequence << std::endl;
    else if (const auto* update = std::get_if<Message>(&notification))
        std::cout << "commit " << update->seq_num << " quantity " << update->quantity << std::endl;
    else if (const auto* status = std::get_if<Receiver::Status>(&notification)) {
        if (status->state == Receiver::State::Running)
            std::cout << (status->known_gap ? "gap open" : "gap closed") << std::endl;
    }
}
} // namespace

int main(int argc, char** argv) try {
    if (argc != 1 && argc != 4)
        throw std::invalid_argument("usage: twinfeed_receiver [a_port b_port replay_port]");
    const auto publisher = loopback::address(argc == 4 ? loopback::parse_port(argv[3]) : 6000);
    if (!publisher.sin_port) throw std::invalid_argument("expected a nonzero publisher replay port");
    const loopback::Socket a(argc == 4 ? loopback::parse_port(argv[1]) : 5001);
    const loopback::Socket b(argc == 4 ? loopback::parse_port(argv[2]) : 5002);
    const loopback::Socket replay(0);
    if (::connect(replay.fd(), reinterpret_cast<const sockaddr*>(&publisher), sizeof(publisher)) < 0)
        throw std::system_error(errno, std::generic_category(), "connect replay socket");

    Receiver receiver(7, report);
    std::cout << "live-ports " << a.port() << ' ' << b.port() << std::endl;
    while (receiver.state() == Receiver::State::Running) {
        const auto request = receive_once(receiver, a.fd(), b.fd(), std::chrono::microseconds(10), replay.fd());
        if (!request) continue;
        loopback::send_packet(replay.fd(), encode_replay_request(*request).value());
        std::cout << "request " << request->first_sequence << ' ' << request->last_sequence << std::endl;
    }
    std::cerr << (receiver.state() == Receiver::State::NeedsRecovery ? "Recovery required" : "Feed conflict")
              << " at committed sequence " << receiver.last_committed_sequence() << '\n';
    return 1;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
