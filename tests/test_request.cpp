#include "http/request.hpp"

#include <string>

#include "doctest/doctest.h"
#include "http/url.hpp"

using http::FormatRequest;
using http::Method;
using http::ParseUrl;

namespace {
std::string AsString(const std::vector<std::uint8_t>& bytes) { return {bytes.begin(), bytes.end()}; }
}  // namespace

TEST_CASE("FormatRequest: GET has no body or Content-Length") {
    const auto url = ParseUrl("http://example.com/x");
    const auto req = AsString(FormatRequest(Method::Get, url, {}, {'i', 'g', 'n', 'o', 'r', 'e', 'd'}));
    CHECK(req.rfind("GET /x HTTP/1.1\r\n", 0) == 0);
    CHECK(req.find("Host: example.com\r\n") != std::string::npos);
    CHECK(req.find("Content-Length:") == std::string::npos);
    CHECK(req.rfind("ignored") == std::string::npos);
}

TEST_CASE("FormatRequest: POST includes body and Content-Length") {
    const auto url = ParseUrl("http://example.com/x");
    const std::vector<std::uint8_t> body = {'h', 'i'};
    const auto req = AsString(FormatRequest(Method::Post, url, {}, body));
    CHECK(req.rfind("POST /x HTTP/1.1\r\n", 0) == 0);
    CHECK(req.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(req.substr(req.size() - 2) == "hi");
}

TEST_CASE("FormatRequest: default User-Agent is generic, not project-identifying") {
    const auto url = ParseUrl("http://example.com/");
    const auto req = AsString(FormatRequest(Method::Get, url, {}, {}));
    CHECK(req.find("User-Agent: Mozilla/5.0") != std::string::npos);
    CHECK(req.find("ra-http-client") == std::string::npos);
    CHECK(req.find("Connection: close\r\n") != std::string::npos);
}

TEST_CASE("FormatRequest: caller-supplied header is forwarded") {
    const auto url = ParseUrl("http://example.com/");
    const auto req = AsString(FormatRequest(Method::Get, url, {{"Authorization", "Bearer t"}}, {}));
    CHECK(req.find("Authorization: Bearer t\r\n") != std::string::npos);
}
