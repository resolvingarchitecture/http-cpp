#pragma once

/// Reads one HTTP/1.1 response off an `IoStream`: status line, headers, and
/// a body determined by `Content-Length`, chunked `Transfer-Encoding`, or
/// (absent both) read-to-EOF - the last of which is only reachable because
/// this client always sends `Connection: close`, so a well-behaved server
/// closes once it has nothing left to send.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "http/io_stream.hpp"
#include "ra_common/exception.hpp"

namespace http {

struct Response {
    int status_code = 0;
    std::string status_text;
    std::map<std::string, std::string> headers;  // lower-cased names
    std::vector<std::uint8_t> body;
};

namespace detail {

inline std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

}  // namespace detail

/// Reads one response off `stream`. `carry` holds bytes already read past
/// the previous response's end on entry (pass an empty vector for the first
/// request on a connection); on return it holds bytes read past *this*
/// response's end, for a caller that pipelines further requests. This client
/// always closes after one response, so callers can just discard it.
inline Response ReadResponse(IoStream& stream, std::vector<std::uint8_t>& carry) {
    auto find_header_end = [](const std::vector<std::uint8_t>& buf) -> std::size_t {
        static const std::string sep = "\r\n\r\n";
        const auto it = std::search(buf.begin(), buf.end(), sep.begin(), sep.end());
        return it == buf.end() ? std::string::npos : static_cast<std::size_t>(it - buf.begin());
    };

    std::vector<std::uint8_t> buf = std::move(carry);
    carry.clear();
    std::uint8_t chunk[16384];

    std::size_t header_end;
    while ((header_end = find_header_end(buf)) == std::string::npos) {
        const std::size_t n = stream.RecvSome(chunk, sizeof(chunk));
        if (n == 0) throw ra::common::RaException(ra::common::RaErrorKind::Io, "connection closed before response headers completed");
        buf.insert(buf.end(), chunk, chunk + n);
    }

    const std::string header_text(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(header_end));
    std::vector<std::uint8_t> after_headers(buf.begin() + static_cast<std::ptrdiff_t>(header_end) + 4, buf.end());

    Response resp;
    std::size_t line_start = 0;
    bool first = true;
    while (line_start < header_text.size()) {
        auto line_end = header_text.find("\r\n", line_start);
        if (line_end == std::string::npos) line_end = header_text.size();
        const std::string line = header_text.substr(line_start, line_end - line_start);
        line_start = line_end + 2;
        if (line.empty()) continue;
        if (first) {
            first = false;
            const auto sp1 = line.find(' ');
            if (sp1 == std::string::npos) throw ra::common::RaException(ra::common::RaErrorKind::Io, "malformed status line: " + line);
            const auto sp2 = line.find(' ', sp1 + 1);
            const std::string code_str = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
            resp.status_code = std::stoi(code_str);
            resp.status_text = sp2 == std::string::npos ? "" : line.substr(sp2 + 1);
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        resp.headers[detail::ToLower(detail::Trim(line.substr(0, colon)))] = detail::Trim(line.substr(colon + 1));
    }

    const auto te_it = resp.headers.find("transfer-encoding");
    const bool chunked = te_it != resp.headers.end() && detail::ToLower(te_it->second).find("chunked") != std::string::npos;

    auto recv_more = [&]() -> bool {
        const std::size_t n = stream.RecvSome(chunk, sizeof(chunk));
        if (n == 0) return false;
        after_headers.insert(after_headers.end(), chunk, chunk + n);
        return true;
    };

    if (chunked) {
        std::size_t pos = 0;
        for (;;) {
            std::size_t line_end;
            for (;;) {
                static const std::string crlf = "\r\n";
                const auto it = std::search(after_headers.begin() + static_cast<std::ptrdiff_t>(pos), after_headers.end(), crlf.begin(), crlf.end());
                if (it != after_headers.end()) {
                    line_end = static_cast<std::size_t>(it - after_headers.begin());
                    break;
                }
                if (!recv_more()) throw ra::common::RaException(ra::common::RaErrorKind::Io, "connection closed mid chunk-size line");
            }
            const std::string size_line(after_headers.begin() + static_cast<std::ptrdiff_t>(pos),
                                         after_headers.begin() + static_cast<std::ptrdiff_t>(line_end));
            const auto semi = size_line.find(';');
            const std::string size_hex = semi == std::string::npos ? size_line : size_line.substr(0, semi);
            const std::size_t chunk_size = size_hex.empty() ? 0 : std::stoul(size_hex, nullptr, 16);
            pos = line_end + 2;

            if (chunk_size == 0) {
                while (after_headers.size() < pos + 2) {
                    if (!recv_more()) break;  // trailer/final CRLF missing - tolerate a hard close here
                }
                break;
            }
            while (after_headers.size() < pos + chunk_size + 2) {
                if (!recv_more()) throw ra::common::RaException(ra::common::RaErrorKind::Io, "connection closed mid chunk body");
            }
            resp.body.insert(resp.body.end(), after_headers.begin() + static_cast<std::ptrdiff_t>(pos),
                              after_headers.begin() + static_cast<std::ptrdiff_t>(pos + chunk_size));
            pos += chunk_size + 2;  // skip the chunk's trailing CRLF
        }
        carry.assign(after_headers.begin() + static_cast<std::ptrdiff_t>(std::min(pos, after_headers.size())), after_headers.end());
    } else {
        const auto cl_it = resp.headers.find("content-length");
        if (cl_it != resp.headers.end()) {
            const std::size_t content_length = std::stoul(cl_it->second);
            while (after_headers.size() < content_length) {
                if (!recv_more()) throw ra::common::RaException(ra::common::RaErrorKind::Io, "connection closed before Content-Length bytes received");
            }
            resp.body.assign(after_headers.begin(), after_headers.begin() + static_cast<std::ptrdiff_t>(content_length));
            carry.assign(after_headers.begin() + static_cast<std::ptrdiff_t>(content_length), after_headers.end());
        } else {
            while (recv_more()) {
            }
            resp.body = std::move(after_headers);
        }
    }
    return resp;
}

}  // namespace http
