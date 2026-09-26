#pragma once

/// A connected TCP socket as an `IoStream`, with a connect-timeout (blocking
/// `::connect` has none). Ports the socket helpers `tor-client-cpp` keeps in
/// `detail.hpp`/`socks.hpp`, wrapped in an RAII type instead of a bare fd.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include "http/io_stream.hpp"
#include "ra_common/exception.hpp"

namespace http {

/// Connects to `host:port` with a connect-timeout, returning the connected
/// fd (blocking mode restored) or -1 on failure/timeout.
inline int ConnectWithTimeout(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || res == nullptr) {
        return -1;
    }

    int fd = -1;
    for (auto* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            ::fcntl(fd, F_SETFL, flags);
            break;
        }
        if (errno != EINPROGRESS) {
            ::close(fd);
            fd = -1;
            continue;
        }

        struct pollfd pfd {
            fd, POLLOUT, 0
        };
        const int pr = ::poll(&pfd, 1, static_cast<int>(timeout.count()));
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (pr <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
            ::close(fd);
            fd = -1;
            continue;
        }
        ::fcntl(fd, F_SETFL, flags);
        break;
    }
    ::freeaddrinfo(res);
    return fd;
}

/// Sets `SO_RCVTIMEO`/`SO_SNDTIMEO` on an already-connected fd.
inline void SetSocketTimeouts(int fd, std::chrono::milliseconds timeout) {
    struct timeval tv {};
    tv.tv_sec = timeout.count() / 1000;
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/// Owns a connected TCP socket fd - closes it on destruction. Move-only.
class TcpStream : public IoStream {
public:
    explicit TcpStream(int fd) : fd_(fd) {}
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    TcpStream& operator=(TcpStream&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    ~TcpStream() override {
        if (fd_ >= 0) ::close(fd_);
    }

    int fd() const { return fd_; }

    void SendAll(const std::uint8_t* data, std::size_t len) override {
        std::size_t sent = 0;
        while (sent < len) {
            const ssize_t n = ::send(fd_, data + sent, len - sent, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw ra::common::RaException(ra::common::RaErrorKind::Io, std::string("send failed: ") + std::strerror(errno));
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    std::size_t RecvSome(std::uint8_t* buf, std::size_t cap) override {
        for (;;) {
            const ssize_t n = ::recv(fd_, buf, cap, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw ra::common::RaException(ra::common::RaErrorKind::Io, std::string("recv failed: ") + std::strerror(errno));
            }
            return static_cast<std::size_t>(n);
        }
    }

    /// Connects with a timeout; throws `ra::common::RaException(Io)` on failure.
    static TcpStream Connect(const std::string& host, std::uint16_t port, std::chrono::milliseconds timeout) {
        const int fd = ConnectWithTimeout(host, port, timeout);
        if (fd < 0) {
            throw ra::common::RaException(ra::common::RaErrorKind::Io, "could not connect to " + host + ":" + std::to_string(port));
        }
        SetSocketTimeouts(fd, timeout);
        return TcpStream(fd);
    }

private:
    int fd_;
};

}  // namespace http
