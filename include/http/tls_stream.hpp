#pragma once

/// TLS over an already-connected TCP socket, via OpenSSL (`libssl-dev`).
/// `libcurl` dev headers aren't available in every build environment this
/// repo targets, so - like `tor-client-cpp`'s SOCKS layer talks raw sockets
/// instead of pulling in a SOCKS library - this talks OpenSSL directly
/// instead of pulling in libcurl. See DESIGN.md.

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <string>

#include "http/io_stream.hpp"
#include "http/tcp_stream.hpp"
#include "ra_common/exception.hpp"

namespace http {

/// Wraps a `TcpStream` in a TLS client session. `trust_all` skips certificate
/// verification entirely (self-signed/test servers only - never for
/// production traffic); otherwise the system default trust store is used and
/// the peer certificate is verified against `sni_host`.
class TlsStream : public IoStream {
public:
    TlsStream(TcpStream tcp, const std::string& sni_host, bool trust_all) : tcp_(std::move(tcp)) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (ctx_ == nullptr) {
            throw ra::common::RaException(ra::common::RaErrorKind::Io, "SSL_CTX_new failed: " + OpensslError());
        }
        if (trust_all) {
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
        } else {
            SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
            if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
                const std::string err = OpensslError();
                SSL_CTX_free(ctx_);
                throw ra::common::RaException(ra::common::RaErrorKind::Io, "SSL_CTX_set_default_verify_paths failed: " + err);
            }
        }

        ssl_ = SSL_new(ctx_);
        if (ssl_ == nullptr) {
            const std::string err = OpensslError();
            SSL_CTX_free(ctx_);
            throw ra::common::RaException(ra::common::RaErrorKind::Io, "SSL_new failed: " + err);
        }
        SSL_set_fd(ssl_, tcp_.fd());
        SSL_set_tlsext_host_name(ssl_, sni_host.c_str());  // SNI
        if (!trust_all) {
            SSL_set1_host(ssl_, sni_host.c_str());  // hostname verification (OpenSSL 1.0.2+)
        }

        const int r = SSL_connect(ssl_);
        if (r != 1) {
            const int ssl_err = SSL_get_error(ssl_, r);
            const std::string detail = OpensslError();
            Cleanup();
            throw ra::common::RaException(
                ra::common::RaErrorKind::Io,
                "TLS handshake with " + sni_host + " failed (SSL_get_error=" + std::to_string(ssl_err) + "): " + detail);
        }
    }

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;
    TlsStream(TlsStream&&) = delete;
    TlsStream& operator=(TlsStream&&) = delete;

    ~TlsStream() override { Cleanup(); }

    void SendAll(const std::uint8_t* data, std::size_t len) override {
        std::size_t sent = 0;
        while (sent < len) {
            const int n = SSL_write(ssl_, data + sent, static_cast<int>(len - sent));
            if (n <= 0) {
                throw ra::common::RaException(ra::common::RaErrorKind::Io, "SSL_write failed: " + OpensslError());
            }
            sent += static_cast<std::size_t>(n);
        }
    }

    std::size_t RecvSome(std::uint8_t* buf, std::size_t cap) override {
        const int n = SSL_read(ssl_, buf, static_cast<int>(cap));
        if (n > 0) return static_cast<std::size_t>(n);
        const int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_ZERO_RETURN) return 0;               // clean TLS close_notify
        if (err == SSL_ERROR_SYSCALL && n == 0) return 0;         // peer closed without close_notify
        throw ra::common::RaException(ra::common::RaErrorKind::Io, "SSL_read failed: " + OpensslError());
    }

private:
    static std::string OpensslError() {
        const unsigned long code = ERR_get_error();
        if (code == 0) return "no further details";
        char buf[256];
        ERR_error_string_n(code, buf, sizeof(buf));
        return buf;
    }

    void Cleanup() {
        if (ssl_ != nullptr) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_ != nullptr) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    TcpStream tcp_;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

}  // namespace http
