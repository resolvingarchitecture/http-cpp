#pragma once

/// `HttpClient` - a direct (non-anonymized) HTTP/HTTPS client. Ports the
/// `sendOut`/client half of `ra.http.HTTPService` (`http-client-java`); no
/// local server/SPA/WebSocket hosting - see DESIGN.md for why that half is
/// out of scope here. One TCP(+TLS) connection per request/redirect hop, no
/// connection pooling (unlike OkHttp in the Java version) - see TODO.md.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ra_common/envelope.hpp"
#include "ra_common/route/external_route.hpp"

#include "http/io_stream.hpp"
#include "http/request.hpp"
#include "http/response.hpp"
#include "http/socks5.hpp"
#include "http/tcp_stream.hpp"
#include "http/tls_stream.hpp"
#include "http/url.hpp"

namespace http {

constexpr std::chrono::milliseconds kDefaultTimeout{60'000};
constexpr int kDefaultMaxRedirects = 5;

enum class Status { Connecting, Connected, Disconnected, Error };

inline const char* ToString(Status s) {
    switch (s) {
        case Status::Connecting: return "Connecting";
        case Status::Connected: return "Connected";
        case Status::Disconnected: return "Disconnected";
        case Status::Error: return "Error";
    }
    return "Disconnected";
}

class HttpClient {
public:
    HttpClient() = default;

    /// Config keys: `ra.http.client.trustAllCerts`, `ra.http.client.timeoutSecs`,
    /// `ra.http.client.followRedirects`, `ra.http.client.maxRedirects`,
    /// `ra.http.client.proxyHost`, `ra.http.client.proxyPort`.
    explicit HttpClient(const std::map<std::string, std::string>& cfg) { ApplyConfig(cfg); }
    static HttpClient FromConfig(const std::map<std::string, std::string>& cfg) { return HttpClient(cfg); }

    bool trust_all_certs = false;
    bool follow_redirects = true;
    int max_redirects = kDefaultMaxRedirects;
    std::chrono::milliseconds timeout = kDefaultTimeout;
    std::optional<std::string> proxy_host;
    std::uint16_t proxy_port = 1080;

    Status GetStatus() const { return status_; }

    bool Start() {
        status_ = Status::Connected;
        return true;
    }
    bool Stop() {
        status_ = Status::Disconnected;
        return true;
    }
    bool IsConnected() const { return status_ == Status::Connected; }

    /// Sends `envelope` (its `url`/`action`/`headers`/content) as one HTTP
    /// request, following redirects if `follow_redirects` and writing the
    /// response body back via `envelope.AddContent`. Returns false and
    /// records `envelope.AddErrorMessage(...)` on any failure - mirrors
    /// `ra.http.HTTPService.sendOut`'s error-reporting contract.
    bool Send(ra::common::Envelope& envelope) {
        if (status_ != Status::Connected) {
            envelope.AddErrorMessage("HTTP Client not connected and unable to connect.");
            return false;
        }

        ResolveUrlFromRouteIfMissing(envelope);
        if (!envelope.url || envelope.url->empty()) {
            envelope.AddErrorMessage("Must provide either a URL or External Route with destination Network Peer.");
            return false;
        }
        if (!envelope.action) {
            envelope.AddErrorMessage("Envelope action must be set to GET, POST, PUT, or DELETE");
            return false;
        }
        const Method method = ToMethod(*envelope.action);

        std::map<std::string, std::string> headers = PickHeaders(envelope);
        std::vector<std::uint8_t> body;
        if (method != Method::Get && !BuildBody(envelope, headers, body)) {
            return false;  // BuildBody already recorded the error
        }

        std::string url = *envelope.url;
        for (int hop = 0;; hop++) {
            ParsedUrl parsed;
            try {
                parsed = ParseUrl(url);
            } catch (const std::exception& e) {
                envelope.AddErrorMessage(e.what());
                return false;
            }

            Response resp;
            try {
                resp = DoRequest(method, parsed, headers, body);
            } catch (const std::exception& e) {
                envelope.AddErrorMessage(e.what());
                return false;
            }

            if (follow_redirects && IsRedirect(resp.status_code) && hop < max_redirects) {
                const auto it = resp.headers.find("location");
                if (it != resp.headers.end() && !it->second.empty()) {
                    url = ResolveRedirect(parsed, it->second);
                    continue;
                }
            }

            envelope.AddContent(nlohmann::json::binary(resp.body));
            if (resp.status_code < 200 || resp.status_code >= 300) {
                envelope.AddErrorMessage(std::to_string(resp.status_code));
                LogBlockedIfKnown(resp.status_code, url);
                return false;
            }
            return true;
        }
    }

private:
    void ApplyConfig(const std::map<std::string, std::string>& cfg) {
        if (auto it = cfg.find("ra.http.client.trustAllCerts"); it != cfg.end()) trust_all_certs = it->second == "true";
        if (auto it = cfg.find("ra.http.client.followRedirects"); it != cfg.end()) follow_redirects = it->second != "false";
        if (auto it = cfg.find("ra.http.client.maxRedirects"); it != cfg.end()) max_redirects = std::stoi(it->second);
        if (auto it = cfg.find("ra.http.client.timeoutSecs"); it != cfg.end()) {
            timeout = std::chrono::milliseconds(std::stoll(it->second) * 1000);
        }
        if (auto it = cfg.find("ra.http.client.proxyHost"); it != cfg.end()) proxy_host = it->second;
        if (auto it = cfg.find("ra.http.client.proxyPort"); it != cfg.end()) {
            proxy_port = static_cast<std::uint16_t>(std::stoi(it->second));
        }
    }

    static Method ToMethod(ra::common::EnvelopeAction a) {
        switch (a) {
            case ra::common::EnvelopeAction::Post: return Method::Post;
            case ra::common::EnvelopeAction::Put: return Method::Put;
            case ra::common::EnvelopeAction::Delete: return Method::Delete;
            case ra::common::EnvelopeAction::Get: return Method::Get;
        }
        return Method::Get;
    }

    static void ResolveUrlFromRouteIfMissing(ra::common::Envelope& envelope) {
        if (envelope.url && !envelope.url->empty()) return;
        auto* route = envelope.GetRoute();
        auto* ext = dynamic_cast<ra::common::route::SimpleExternalRoute*>(route);
        if (ext != nullptr && ext->destination && ext->destination->id) {
            envelope.url = "http://" + *ext->destination->id;
        }
    }

    /// Picks the same header set `ra.http.HTTPService` forwards.
    static std::map<std::string, std::string> PickHeaders(const ra::common::Envelope& envelope) {
        using ra::common::HeaderNames;
        std::map<std::string, std::string> out;
        for (const char* name : {HeaderNames::kAuthorization, HeaderNames::kContentDisposition,
                                  HeaderNames::kContentType, HeaderNames::kContentTransferEncoding,
                                  HeaderNames::kUserAgent}) {
            const auto v = envelope.Header(name);
            if (v.is_string()) out[name] = v.get<std::string>();
        }
        return out;
    }

    /// Builds the request body for POST/PUT/DELETE. Prefers `Multipart` if
    /// set; otherwise, for a `send_content_only` external route, sends the
    /// document content directly (string or binary); otherwise falls back to
    /// the whole envelope as JSON - same three-way choice `HTTPService.
    /// sendOut` makes. Returns false (with `envelope.AddErrorMessage`
    /// already called) if content was present but of an unsupported type.
    static bool BuildBody(ra::common::Envelope& envelope, std::map<std::string, std::string>& headers,
                           std::vector<std::uint8_t>& body) {
        if (envelope.multipart) {
            headers[ra::common::HeaderNames::kContentType] = "multipart/form-data; boundary=" + envelope.multipart->boundary();
            const std::string finished = envelope.multipart->Finish();
            body.assign(finished.begin(), finished.end());
            return true;
        }

        auto* route = envelope.GetRoute();
        auto* ext = dynamic_cast<ra::common::route::SimpleExternalRoute*>(route);
        if (ext != nullptr && ext->send_content_only) {
            const auto content = envelope.Content();
            if (content.is_string()) {
                const auto& s = content.get_ref<const std::string&>();
                body.assign(s.begin(), s.end());
                return true;
            }
            if (content.is_binary()) {
                const auto& b = content.get_binary();
                body.assign(b.begin(), b.end());
                return true;
            }
            envelope.AddErrorMessage("Only string or binary content supported for send-content-only.");
            return false;
        }

        const std::string json = envelope.ToJsonString(-1);  // -1 = compact (nlohmann::json::dump default)
        body.assign(json.begin(), json.end());
        return true;
    }

    static bool IsRedirect(int status) {
        return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
    }

    /// Resolves a `Location` header against the request it answered.
    /// Supports absolute URLs and root-relative paths; anything else (rare
    /// in practice) is treated as opaque and left unresolved - see TODO.md.
    static std::string ResolveRedirect(const ParsedUrl& from, const std::string& location) {
        if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) return location;
        if (!location.empty() && location.front() == '/') {
            return std::string(from.https ? "https://" : "http://") + from.host +
                   (from.port == (from.https ? 443 : 80) ? "" : ":" + std::to_string(from.port)) + location;
        }
        return location;
    }

    Response DoRequest(Method method, const ParsedUrl& url, const std::map<std::string, std::string>& headers,
                        const std::vector<std::uint8_t>& body) {
        TcpStream tcp = proxy_host ? ConnectThroughSocks5(*proxy_host, proxy_port, url.host, url.port, timeout)
                                    : TcpStream::Connect(url.host, url.port, timeout);
        std::vector<std::uint8_t> carry;
        const auto req = FormatRequest(method, url, headers, body);
        if (url.https) {
            TlsStream tls(std::move(tcp), url.host, trust_all_certs);
            tls.SendAll(req.data(), req.size());
            return ReadResponse(tls, carry);
        }
        tcp.SendAll(req.data(), req.size());
        return ReadResponse(tcp, carry);
    }

    /// No `NetworkConnectionReport` type exists in `ra-common-cpp` yet (see
    /// `network.hpp`), so - unlike the Java version, which raises one per
    /// code - this just logs the same "likely blocked" interpretation.
    static void LogBlockedIfKnown(int status, const std::string& url) {
        const char* reason = nullptr;
        switch (status) {
            case 403: reason = "BLOCKED-FORBIDDEN"; break;
            case 408: reason = "BLOCKED-TIMEOUT"; break;
            case 410: reason = "BLOCKED-GONE"; break;
            case 418: reason = "BLOCKED-TEAPOT"; break;
            case 451: reason = "BLOCKED-LEGAL"; break;
            case 511: reason = "BLOCKED-AUTHN"; break;
            default: return;
        }
        std::cerr << "http: " << url << " -> " << status << " (" << reason << ")\n";
    }

    Status status_ = Status::Disconnected;
};

}  // namespace http
