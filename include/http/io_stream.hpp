#pragma once

/// Minimal byte-stream abstraction so request/response code works the same
/// whether the underlying transport is a plain TCP socket or a TLS session.

#include <cstddef>
#include <cstdint>

namespace http {

class IoStream {
public:
    virtual ~IoStream() = default;

    /// Sends all of `data`/`len`, looping until everything is written.
    virtual void SendAll(const std::uint8_t* data, std::size_t len) = 0;

    /// Reads at most `cap` bytes into `buf`. Returns 0 on a clean EOF.
    virtual std::size_t RecvSome(std::uint8_t* buf, std::size_t cap) = 0;
};

}  // namespace http
