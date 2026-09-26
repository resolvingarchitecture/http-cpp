# http (C++) — Design

A direct HTTP/HTTPS client for use as the HTTP **protocol service** by a
future `1m5-core-cpp` — the clearnet fallback alongside `i2p-cpp` and
`tor-client-cpp`. A C++ port of `ra.http.HTTPService`
(`http-java`), client half only.

## Where it sits

    (future) 1m5-core-cpp  ──wraps──►  http::HttpClient
                                               │
                                   TCP(+TLS) per request/hop
                                               │
                                        the target server

`tor-client-cpp::TorClient` could, in principle, depend on this library and
delegate through its `proxy_host`/`proxy_port` instead of maintaining its
own `socks.hpp` + `http.hpp`. Not done in this pass — this library came
after `tor-client-cpp` and changing that one's dependency graph is out of
scope here. `socks5.hpp` in this repo is a standalone copy of the same
handshake so this library has no dependency on `tor-client-cpp` (layering
runs client-under-Tor, not the other way — mirrors `TORClientService extends
HTTPService` in `http-java`/`tor-java`).

## Why OpenSSL, not libcurl

`http-java` uses OkHttp. The natural C++ analogue for a
full-featured HTTP+TLS client is libcurl, but `libcurl`'s *development*
headers (`curl/curl.h`) aren't guaranteed present in every environment this
repo builds in — only runtime `.so`s are, in general — whereas `libssl-dev`
(OpenSSL) is a much safer bet, and it's the same TLS reasoning
`tor-client-cpp`'s `DESIGN.md` already names ("HTTPS needs OpenSSL or
another TLS library wrapped around the SOCKS fd", never done there). So the
whole HTTP layer — TCP connect, TLS handshake, request formatting, response
parsing (status line, headers, `Content-Length` / chunked / read-to-EOF
bodies), redirect following — is hand-rolled on POSIX sockets + OpenSSL, no
HTTP library dependency at all. Same dependency-light default every other
C++ port in this monorepo uses.

## Components

    io_stream    `IoStream` - the byte-stream interface request/response
                 code is written against (`SendAll`/`RecvSome`), so the same
                 code works over a plain socket or a TLS session
    tcp_stream   `TcpStream` - RAII fd, `ConnectWithTimeout` (blocking
                 `::connect` has none), `SO_RCVTIMEO`/`SO_SNDTIMEO`;
                 ports `tor-client-cpp/detail.hpp`
    tls_stream   `TlsStream` - OpenSSL `SSL_CTX`/`SSL` over an already-
                 connected `TcpStream`; SNI, hostname verification (or
                 `SSL_VERIFY_NONE` for `trust_all_certs`)
    socks5       `ConnectThroughSocks5` - minimal SOCKS5 CONNECT (no auth);
                 a standalone copy of `tor-client-cpp/socks.hpp`'s handshake
    url          `ParseUrl` - scheme/host/port/path, with the
                 scheme-appropriate default port
    request      `FormatRequest` - GET sends no body (matches OkHttp's
                 `Request.Builder.get()`); POST/PUT/DELETE send `body` with
                 a `Content-Length`
    response     `ReadResponse` - status line, headers (lower-cased), body
                 via `Content-Length`, chunked `Transfer-Encoding`, or
                 read-to-EOF (reachable because this client always sends
                 `Connection: close`)
    http  `HttpClient` - config, status, `Start()`/`Stop()`/`Send()`,
                 redirect following

Header-only (matches `ra-common-cpp`/`tor-client-cpp`/`i2p-cpp`'s
POSIX-target C++ ports); POSIX sockets, same reasoning as `tor-client-cpp`.

## Message flow

**Outbound** — `Send(envelope)` reads `envelope.url` (or, if unset, an
`ExternalRoute`'s `destination.id` as an `http://` host — mirrors
`HTTPService.sendOut`'s fallback) and `envelope.action`
(`Get`/`Post`/`Put`/`Delete`; unset is an error, matching the Java
`default:` branch). Request headers are picked from `envelope.headers` -
only the same five `HeaderNames` the Java version forwards (`Authorization`,
`Content-Type`, `Content-Disposition`, `Content-Transfer-Encoding`,
`User-Agent`). The request body (POST/PUT/DELETE only — GET never sends
one) is chosen the same three-way way `HTTPService.sendOut` does:

1. `envelope.multipart` set → its `Finish()`ed body, `Content-Type`
   overridden to `multipart/form-data; boundary=...`.
2. Else, an `ExternalRoute` with `send_content_only` → the document content
   directly (string or binary; anything else is an error).
3. Else → the whole envelope, JSON-serialized (`envelope.ToJsonString(-1)`),
   matching Java's unconditional `e.toJSON()` fallback.

The response body is written back via `envelope.AddContent(nlohmann::json::
binary(body))`. A non-2xx status calls `envelope.AddErrorMessage(status)`
and — like `HTTPService.handleFailure`, but without constructing a formal
report object (`ra-common-cpp` has no `NetworkConnectionReport` type yet;
see `network.hpp`) — logs the same "likely blocked" interpretation for
403/408/410/418/451/511 to stderr.

**Redirects** — 301/302/303/307/308 are followed (up to `max_redirects`,
default 5) when `follow_redirects` is set (default `true`), same as
OkHttp's default in the Java client. Only absolute and root-relative
`Location` values are resolved; anything else is left as-is (rare in
practice for a same-origin redirect — see TODO.md).

**Inbound** — not implemented; this is the client half only (see "Not
here").

## Status model

`Status` is its own 4-state enum (`Connecting`, `Connected`, `Disconnected`,
`Error`), matching `tor_client::Status`/`i2p_client`'s convention rather
than `ra_common::service::ServiceStatus`. Unlike `TorClient`, `HttpClient`
does not probe anything on `Start()` — there's no daemon to reach, so it
just flips to `Connected`; the real failure mode is per-request (a bad
host, a TLS failure, a non-2xx status), reported through `Send`'s return
value and `envelope.AddErrorMessage`, not through `Status`.

## No connection pooling

Each `Send()` call (and each redirect hop within it) opens its own
TCP(+TLS) connection and always sends `Connection: close`. `HTTPService`'s
OkHttp clients keep a connection pool warm across calls; this port doesn't
attempt that — a fresh handshake per request is simpler and was enough to
pass the same live-network test the Java suite runs (`HTTPServiceTest.
httpClientTest`/`httpsClientTest`, ported here as the `live:` test cases).
Worth revisiting if this ever sits in a hot path with high request volume
(see TODO.md).

## C++ adaptations vs. the other ports

- **`IoStream` abstracts transport**, not present in any of the other
  language ports (none of them needed to share request/response code
  between a plain and a TLS transport the way this one does). `TcpStream`
  and `TlsStream` both implement it; `FormatRequest`/`ReadResponse` are
  written against the interface, not either concrete type.
- **`ra::common::RaException`** for errors, `RaErrorKind::Io` for
  connection/protocol/TLS failures, `RaErrorKind::Invalid` for bad input —
  same package-wide convention `tor-client-cpp` uses.
- **Blocking POSIX sockets** via `SO_RCVTIMEO`, same as `tor-client-cpp` —
  no listener-lifecycle gotcha the way `tor-client-ts`'s `SocketReader`
  has to work around.
- Real request-line-based routing, not header-smuggled: `envelope.url` and
  `envelope.action` are first-class `ra::common::Envelope` fields in the
  C++ port (unlike the `envelope.headers["url"]` workaround
  `tor-client-cpp`/`-python`/`-ts` use, needed there only because those
  clients predate this library and never revisited it).

## Identity metadata leaks

Required standard for any HTTP client this project relies on for anonymized
traffic (Tor/I2P), enforced here and checked against every sibling
`http-*` port: no default header, response header, or connection
behavior may reveal more about the requester than it has to. Two concrete
bug shapes this actually takes, found via direct source/bytecode inspection
of this project's own clients (not theoretical):

- **A project- or library-identifying default `User-Agent`.**
  `FormatRequest` used to send `User-Agent: ra-http-client` whenever a
  caller didn't set one - fixed 2026-09-26 to a generic, widely-shared
  browser value instead. The equivalent bug was found and fixed the same
  day in `http-java` (OkHttp's `BridgeInterceptor` injects
  `User-Agent: okhttp/<version>` by default - confirmed by disassembling
  its actual bytecode) and in `http-python` (same literal
  `"ra-http-client"` default). A generic value doesn't just hide version
  info - Tor Browser's entire fingerprinting defense rests on every user
  presenting an *identical* signature; a bespoke one defeats that even if
  it reveals nothing else.
- **Local DNS resolution when routed through a proxy.** `ConnectThroughSocks5`
  was checked directly (not assumed) and is correct: it sends `dest_host` as
  a raw SOCKS5 domain-name (`ATYP=3`) request, and only ever calls
  `getaddrinfo` on `proxy_host` (the proxy's own address, safe to resolve
  locally) - never on the actual destination. This is the same requirement
  that was violated and fixed in `bitcoin-client-java`'s bitcoinj DNS-seed
  lookups (`tor-java`, 2026-09-25) and is worth re-confirming after
  any change to `socks5.hpp`.

A third bug shape - a server-identifying response header (`Server:
Jetty(<version>)`, found and fixed in `http-java`'s Jetty-based
inbound listener) doesn't apply here: this client is outbound-only, no
server/inbound half exists (see "Not here" below). If an inbound listener
is ever added, it needs the same check before use.

## Not here

- Connection pooling / keep-alive reuse across requests (see above).
- A formal blocked-response report type (`NetworkConnectionReport` —
  `ra-common-cpp` doesn't have one yet; only logged to stderr today).
- Cookie handling, HTTP/2, compression (`Accept-Encoding`/`Content-Encoding`).
- Non-absolute, non-root-relative redirect `Location` resolution.
- Local server / SPA / WebSocket hosting (`EnvelopeHandler`, `SPAHandler`,
  `EnvelopeWebSocket`, `EnvelopeJSONDataHandler`,
  `EnvelopeProxyDataHandler` in the Java version) — no other language port
  needs this half, and neither does a future `1m5-core-cpp`'s protocol
  service; the Java version's server hosting exists for
  `1m5-desktop-java`'s local RPC API and Tor hidden-service handling
  specifically.
- Windows/macOS socket portability (POSIX-only, matching the rest of this
  monorepo's C++ ports).
