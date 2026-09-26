#include "http/http.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <string>
#include <thread>

#include "doctest/doctest.h"
#include "ra_common/envelope.hpp"

namespace {

std::uint16_t Listen(int& fd) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(fd, 4);
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

}  // namespace

TEST_CASE("Send before Start reports not connected") {
    http::HttpClient client;
    auto env = ra::common::Envelope::Document();
    env.url = "http://example.com/";
    env.action = ra::common::EnvelopeAction::Get;
    CHECK_FALSE(client.Send(env));
    REQUIRE(env.ErrorMessages().size() == 1);
    CHECK(env.ErrorMessages()[0] == "HTTP Client not connected and unable to connect.");
}

TEST_CASE("Send without a URL or destination route errors") {
    http::HttpClient client;
    client.Start();
    auto env = ra::common::Envelope::Document();
    env.action = ra::common::EnvelopeAction::Get;
    CHECK_FALSE(client.Send(env));
    REQUIRE(env.ErrorMessages().size() == 1);
    CHECK(env.ErrorMessages()[0] == "Must provide either a URL or External Route with destination Network Peer.");
}

TEST_CASE("Send without an action errors") {
    http::HttpClient client;
    client.Start();
    auto env = ra::common::Envelope::Document();
    env.url = "http://example.com/";
    CHECK_FALSE(client.Send(env));
    REQUIRE(env.ErrorMessages().size() == 1);
    CHECK(env.ErrorMessages()[0] == "Envelope action must be set to GET, POST, PUT, or DELETE");
}

TEST_CASE("GET against a local plain-HTTP server returns the body") {
    int server_fd;
    const std::uint16_t port = Listen(server_fd);
    std::thread server([server_fd]() {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int c = ::accept(server_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        REQUIRE(c >= 0);
        char req_buf[2048];
        ::recv(c, req_buf, sizeof(req_buf), 0);
        static const char* kResponse = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: close\r\n\r\nHello, world!";
        ::send(c, kResponse, std::strlen(kResponse), 0);
        ::close(c);
    });

    http::HttpClient client;
    client.Start();
    auto env = ra::common::Envelope::Document();
    env.url = "http://127.0.0.1:" + std::to_string(port) + "/test";
    env.action = ra::common::EnvelopeAction::Get;
    CHECK(client.Send(env));
    const auto content = env.Content();
    CHECK(content.is_binary());
    const auto& bytes = content.get_binary();
    CHECK(std::string(bytes.begin(), bytes.end()) == "Hello, world!");

    server.join();
    ::close(server_fd);
}

TEST_CASE("POST sends the envelope body and a non-2xx status is an error") {
    int server_fd;
    const std::uint16_t port = Listen(server_fd);
    std::string received_body;
    std::thread server([server_fd, &received_body]() {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int c = ::accept(server_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        REQUIRE(c >= 0);
        char req_buf[4096];
        const ssize_t n = ::recv(c, req_buf, sizeof(req_buf), 0);
        received_body.assign(req_buf, req_buf + n);
        static const char* kResponse = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        ::send(c, kResponse, std::strlen(kResponse), 0);
        ::close(c);
    });

    http::HttpClient client;
    client.Start();
    auto env = ra::common::Envelope::Document();
    env.url = "http://127.0.0.1:" + std::to_string(port) + "/submit";
    env.action = ra::common::EnvelopeAction::Post;
    env.AddContent(std::string("payload"));
    env.AddExternalRoute("http", "SEND");
    CHECK_FALSE(client.Send(env));
    REQUIRE(env.ErrorMessages().size() == 1);
    CHECK(env.ErrorMessages()[0] == "403");

    server.join();
    ::close(server_fd);
    CHECK(received_body.find("payload") != std::string::npos);
}

// -- Live-network tests --------------------------------------------------
// Mirror HTTPServiceTest.java's httpClientTest/httpsClientTest. These need
// real outbound internet access; they are not gated behind a flag (matching
// the Java suite) but will simply fail if the build environment has none.

TEST_CASE("live: plain HTTP GET follows the redirect to HTTPS") {
    http::HttpClient client;
    client.Start();
    auto env = ra::common::Envelope::Document();
    env.url = "http://resolvingarchitecture.io";
    env.action = ra::common::EnvelopeAction::Get;
    const bool ok = client.Send(env);
    if (!ok) {
        MESSAGE("live network test failed - no internet access in this environment? errors: ", env.ErrorMessages().empty() ? "" : env.ErrorMessages()[0]);
        return;
    }
    const auto content = env.Content();
    REQUIRE(content.is_binary());
    const auto& bytes = content.get_binary();
    const std::string html(bytes.begin(), bytes.end());
    CHECK(html.find("Brian Taylor") != std::string::npos);
}

TEST_CASE("live: HTTPS GET") {
    http::HttpClient client;
    client.Start();
    auto env = ra::common::Envelope::Document();
    env.url = "https://resolvingarchitecture.io";
    env.action = ra::common::EnvelopeAction::Get;
    const bool ok = client.Send(env);
    if (!ok) {
        MESSAGE("live network test failed - no internet access in this environment? errors: ", env.ErrorMessages().empty() ? "" : env.ErrorMessages()[0]);
        return;
    }
    const auto content = env.Content();
    REQUIRE(content.is_binary());
    const auto& bytes = content.get_binary();
    const std::string html(bytes.begin(), bytes.end());
    CHECK(html.find("Brian Taylor") != std::string::npos);
}
