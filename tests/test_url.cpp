#include "http/url.hpp"

#include "doctest/doctest.h"

using http::ParseUrl;

TEST_CASE("ParseUrl: http with default port") {
    const auto u = ParseUrl("http://example.com/path");
    CHECK_FALSE(u.https);
    CHECK(u.host == "example.com");
    CHECK(u.port == 80);
    CHECK(u.path == "/path");
}

TEST_CASE("ParseUrl: https with default port and root path") {
    const auto u = ParseUrl("https://example.com");
    CHECK(u.https);
    CHECK(u.host == "example.com");
    CHECK(u.port == 443);
    CHECK(u.path == "/");
}

TEST_CASE("ParseUrl: explicit port") {
    const auto u = ParseUrl("http://example.com:8080/a/b");
    CHECK(u.port == 8080);
    CHECK(u.path == "/a/b");
}

TEST_CASE("ParseUrl: unsupported scheme throws") {
    CHECK_THROWS(ParseUrl("ftp://example.com"));
}

TEST_CASE("ParseUrl: bad port throws") {
    CHECK_THROWS(ParseUrl("http://example.com:notaport/"));
}
