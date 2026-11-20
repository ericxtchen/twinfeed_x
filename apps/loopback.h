#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace loopback {

inline std::uint16_t parse_port(std::string_view text) {
    std::uint16_t port{};
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, port);
    if (result.ec != std::errc{} || result.ptr != end)
        throw std::invalid_argument("port must be in [0, 65535]");
    return port;
}

inline sockaddr_in address(std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

class Socket {
public:
    explicit Socket(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) {
        if (fd_ < 0) throw std::system_error(errno, std::generic_category(), "socket");
        const auto addr = address(port);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
            const int error = errno;
            ::close(fd_);
            throw std::system_error(error, std::generic_category(), "bind loopback UDP socket");
        }
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { ::close(fd_); }

    int fd() const { return fd_; }

    std::uint16_t port() const {
        sockaddr_in addr{};
        socklen_t size = sizeof(addr);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &size) < 0)
            throw std::system_error(errno, std::generic_category(), "getsockname");
        return ntohs(addr.sin_port);
    }

private:
    int fd_;
};

// A connected UDP socket uses its configured peer when destination is null.
inline void send_packet(int fd, std::span<const std::byte> bytes,
                        const sockaddr_in* destination = nullptr) {
    ssize_t sent;
    do {
        sent = ::sendto(fd, bytes.data(), bytes.size(), 0,
                        reinterpret_cast<const sockaddr*>(destination),
                        destination ? sizeof(*destination) : 0);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) throw std::system_error(errno, std::generic_category(), "sendto");
    if (static_cast<std::size_t>(sent) != bytes.size())
        throw std::runtime_error("incomplete UDP datagram send");
}

} // namespace loopback
