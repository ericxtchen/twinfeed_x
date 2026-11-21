#include "replay_service.h"

#include <iostream>

// a starter main fn that emits some packets and drops the fourth one if signaled
int main(int argc, char** argv) try {
    const bool drop_four = argc > 1 && std::string_view(argv[argc - 1]) == "--drop-four";
    const int port_args = argc - 1 - static_cast<int>(drop_four);
    if (port_args != 0 && port_args != 3)
        throw std::invalid_argument("usage: twinfeed_publisher [a_port b_port replay_port] [--drop-four]");
    const auto a = loopback::address(port_args ? loopback::parse_port(argv[1]) : 5001);
    const auto b = loopback::address(port_args ? loopback::parse_port(argv[2]) : 5002);
    if (!a.sin_port || !b.sin_port || a.sin_port == b.sin_port)
        throw std::invalid_argument("expected distinct, nonzero live destination ports");
    const loopback::Socket replay(port_args ? loopback::parse_port(argv[3]) : 6000);
    const loopback::Socket live(0);

    bool drop_live = false;
    publisher::Feed source(7, 64, [&](Feed leg, const WireMessage& packet) {
        if (!drop_live) loopback::send_packet(live.fd(), packet, leg == Feed::A ? &a : &b);
    });
    std::cout << "replay-port " << replay.port() << std::endl;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        // Fault injection happens after Feed retains the authoritative frame.
        drop_live = drop_four && i == 4;
        source.publish({.session_id = 0, .seq_num = 0, .price_ticks = 100, .quantity = i,
                        .instrument_id = 101, .side = Side::BID, .operation = Operation::UPSERT_LEVEL}).value();
    }
    for (;;) publisher::reply_once(replay.fd(), source);
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
