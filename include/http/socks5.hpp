#pragma once

/// Minimal SOCKS5 CONNECT client (no auth), so `HttpClient` can optionally be
/// routed through a local SOCKS proxy - the same handshake
/// `tor-client-cpp`'s `socks.hpp` implements, ported here as its own copy
/// (this library has no dependency on `tor-client-cpp`; layering runs the
/// other way, same as `TORClientService extends HTTPService` in the Java
/// port - see DESIGN.md) so a future `tor-client-cpp` refactor could depend
/// on this instead of duplicating it.

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "http/tcp_stream.hpp"
#include "ra_common/exception.hpp"

namespace http {

namespace detail {
/// Reads exactly `n` bytes from `fd` (already has `SO_RCVTIMEO` set).
inline std::vector<std::uint8_t> RecvExact(int fd, std::size_t n) {
    std::vector<std::uint8_t> buf(n);
    std::size_t got = 0;
    while (got < n) {
        const ssize_t r = ::recv(fd, buf.data() + got, n - got, 0);
        if (r == 0) throw ra::common::RaException(ra::common::RaErrorKind::Io, "SOCKS5 proxy closed connection early");
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw ra::common::RaException(ra::common::RaErrorKind::Io, "SOCKS5 read timeout");
            }
            throw ra::common::RaException(ra::common::RaErrorKind::Io, std::string("recv failed: ") + std::strerror(errno));
        }
        got += static_cast<std::size_t>(r);
    }
    return buf;
}
}  // namespace detail

/// Opens a TCP connection to `dest_host:dest_port` *through* the SOCKS5
/// proxy at `proxy_host:proxy_port`.
inline TcpStream ConnectThroughSocks5(const std::string& proxy_host, std::uint16_t proxy_port,
                                        const std::string& dest_host, std::uint16_t dest_port,
                                        std::chrono::milliseconds timeout) {
    TcpStream stream = TcpStream::Connect(proxy_host, proxy_port, timeout);
    const int fd = stream.fd();

    // greeting: VER=5, NMETHODS=1, METHOD=0 (no auth)
    const std::uint8_t greeting[3] = {0x05, 0x01, 0x00};
    stream.SendAll(greeting, sizeof(greeting));
    const auto method = detail::RecvExact(fd, 2);
    if (method[0] != 0x05 || method[1] != 0x00) {
        throw ra::common::RaException(ra::common::RaErrorKind::Io, "SOCKS5 proxy refused no-auth");
    }

    // request: VER=5, CMD=1 (connect), RSV=0, ATYP=3 (domain), len, name, port
    if (dest_host.size() > 255) throw ra::common::RaException::Invalid("host too long for SOCKS5");
    std::vector<std::uint8_t> req;
    req.reserve(5 + dest_host.size() + 2);
    req.push_back(0x05);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x03);
    req.push_back(static_cast<std::uint8_t>(dest_host.size()));
    req.insert(req.end(), dest_host.begin(), dest_host.end());
    req.push_back(static_cast<std::uint8_t>(dest_port >> 8));
    req.push_back(static_cast<std::uint8_t>(dest_port & 0xff));
    stream.SendAll(req.data(), req.size());

    // reply: VER, REP, RSV, ATYP, BND.ADDR, BND.PORT
    const auto head = detail::RecvExact(fd, 4);
    if (head[1] != 0x00) {
        throw ra::common::RaException(ra::common::RaErrorKind::Io,
                                       "SOCKS5 connect failed, REP=" + std::to_string(static_cast<int>(head[1])));
    }
    std::size_t bnd_len;
    switch (head[3]) {
        case 0x01: bnd_len = 4; break;
        case 0x04: bnd_len = 16; break;
        case 0x03: bnd_len = detail::RecvExact(fd, 1)[0]; break;
        default:
            throw ra::common::RaException(ra::common::RaErrorKind::Io,
                                           "SOCKS5 bad ATYP " + std::to_string(static_cast<int>(head[3])));
    }
    detail::RecvExact(fd, bnd_len + 2);
    return stream;
}

}  // namespace http
