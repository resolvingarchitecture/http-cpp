#pragma once

/// Splits `scheme`, `host`, `port` and `path` out of an `http://`/`https://`
/// URL. Ports `tor-client-cpp`'s `ParseUrl` (`http.hpp`), extended with an
/// `https` flag and a scheme-appropriate default port.

#include <cctype>
#include <cstdint>
#include <string>

#include "ra_common/exception.hpp"

namespace http {

struct ParsedUrl {
    bool https = false;
    std::string host;
    std::uint16_t port = 80;
    std::string path = "/";
};

inline ParsedUrl ParseUrl(const std::string& url) {
    bool https;
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        https = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        https = false;
        rest = url.substr(7);
    } else {
        throw ra::common::RaException::Invalid("only http:// and https:// URLs are supported: " + url);
    }

    const auto slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (path.empty()) path = "/";
    if (authority.empty()) throw ra::common::RaException::Invalid("missing host in URL: " + url);

    std::uint16_t port = https ? 443 : 80;
    std::string host = authority;
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const std::string port_str = authority.substr(colon + 1);
        if (port_str.empty() || !std::all_of(port_str.begin(), port_str.end(), [](unsigned char c) { return std::isdigit(c); })) {
            throw ra::common::RaException::Invalid("bad port in URL: " + url);
        }
        const int p = std::stoi(port_str);
        if (p < 0 || p > 65535) throw ra::common::RaException::Invalid("bad port in URL: " + url);
        port = static_cast<std::uint16_t>(p);
    }
    if (host.empty()) throw ra::common::RaException::Invalid("missing host in URL: " + url);
    return ParsedUrl{https, host, port, path};
}

}  // namespace http
