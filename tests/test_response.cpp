#include "http/response.hpp"

#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "http/io_stream.hpp"

namespace {

/// Feeds fixed bytes to `ReadResponse` without a real socket, one `chunk_size`
/// slice per `RecvSome` call (default: everything at once) so both the
/// single-read and needs-more-data paths get exercised.
class MemoryStream : public http::IoStream {
public:
    explicit MemoryStream(std::vector<std::uint8_t> data, std::size_t chunk_size = 0)
        : data_(std::move(data)), chunk_size_(chunk_size == 0 ? data_.size() + 1 : chunk_size) {}

    void SendAll(const std::uint8_t*, std::size_t) override {}

    std::size_t RecvSome(std::uint8_t* buf, std::size_t cap) override {
        if (pos_ >= data_.size()) return 0;
        const std::size_t n = std::min({cap, chunk_size_, data_.size() - pos_});
        std::copy(data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n), buf);
        pos_ += n;
        return n;
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t chunk_size_;
    std::size_t pos_ = 0;
};

std::vector<std::uint8_t> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST_CASE("ReadResponse: Content-Length body") {
    MemoryStream stream(Bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain\r\n\r\nhello"));
    std::vector<std::uint8_t> carry;
    const auto resp = http::ReadResponse(stream, carry);
    CHECK(resp.status_code == 200);
    CHECK(resp.status_text == "OK");
    CHECK(resp.headers.at("content-type") == "text/plain");
    CHECK(std::string(resp.body.begin(), resp.body.end()) == "hello");
}

TEST_CASE("ReadResponse: chunked body, arriving in small pieces") {
    MemoryStream stream(Bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                               "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"),
                         /*chunk_size=*/7);
    std::vector<std::uint8_t> carry;
    const auto resp = http::ReadResponse(stream, carry);
    CHECK(resp.status_code == 200);
    CHECK(std::string(resp.body.begin(), resp.body.end()) == "Wikipedia");
}

TEST_CASE("ReadResponse: no Content-Length, no chunking -> read to EOF") {
    MemoryStream stream(Bytes("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nthe rest of the stream"));
    std::vector<std::uint8_t> carry;
    const auto resp = http::ReadResponse(stream, carry);
    CHECK(std::string(resp.body.begin(), resp.body.end()) == "the rest of the stream");
}

TEST_CASE("ReadResponse: headers are lower-cased and trimmed") {
    MemoryStream stream(Bytes("HTTP/1.1 404 Not Found\r\nX-Custom:  value  \r\nContent-Length: 0\r\n\r\n"));
    std::vector<std::uint8_t> carry;
    const auto resp = http::ReadResponse(stream, carry);
    CHECK(resp.status_code == 404);
    CHECK(resp.status_text == "Not Found");
    CHECK(resp.headers.at("x-custom") == "value");
    CHECK(resp.body.empty());
}
