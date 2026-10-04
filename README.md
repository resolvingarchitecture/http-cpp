# http (C++)

A direct (non-anonymized) HTTP/HTTPS client for **1M5**: real TLS via
OpenSSL, no proxy required (an optional SOCKS5 proxy is supported — see
below). A C++ port of [`http-java`](https://github.com/resolvingarchitecture/http-java)'s
`ra.http.HTTPService` — client (`sendOut`) half only, see `DESIGN.md`.
Header-only, C++20, POSIX sockets + OpenSSL.

`tor-client-cpp`'s `http.hpp` is a deliberately tiny `http://`-only GET
helper for tunneling through a Tor SOCKS proxy. This library is the fuller
client: GET/POST/PUT/DELETE, HTTPS, redirects, chunked responses — and its
optional SOCKS5 proxy support means `tor-client-cpp` could eventually depend
on this instead of hand-rolling its own socket code (not done yet — layering
runs the other way today, matching `TORClientService extends HTTPService`
in the Java port).

## Use

```cpp
#include "http/http.hpp"

http::HttpClient client;                 // or HttpClient(cfg)
client.Start();

auto env = ra::common::Envelope::Document();
env.url = "https://example.com/";
env.action = ra::common::EnvelopeAction::Get;
if (client.Send(env)) {
    auto content = env.Content();                // nlohmann::json binary
    auto bytes = content.get_binary();
}
```

### Config keys

| key | default | meaning |
|-----|---------|---------|
| `ra.http.client.trustAllCerts` | `false` | skip TLS certificate verification (test-only) |
| `ra.http.client.followRedirects` | `true` | follow 301/302/303/307/308 |
| `ra.http.client.maxRedirects` | `5` | redirect hop limit |
| `ra.http.client.timeoutSecs` | `60` | connect + read timeout |
| `ra.http.client.proxyHost` / `ra.http.client.proxyPort` | unset | optional SOCKS5 proxy |

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Depends on `ra-common-cpp` (pulled in via relative `add_subdirectory` from
`../../common/ra-common-cpp`) and system OpenSSL (`libssl-dev` — no
`libcurl` dev headers required; see `DESIGN.md`).

Two tests hit the real `resolvingarchitecture.dev` over the network
(mirrors `HTTPServiceTest.java`); they report via `MESSAGE` and return early
rather than failing the suite if the build environment has no outbound
internet access.

## Identity metadata leaks

Checked and fixed (2026-09-26), the same class of bug found and fixed in
`http-java`'s OkHttp-based client: `FormatRequest`'s default
`User-Agent` used to be the literal string `"ra-http-client"` - itself a
fingerprinting leak (it identifies exactly which project made the request,
an even smaller anonymity set than a generic library name) whenever a
caller didn't supply its own. Now defaults to a generic, widely-shared
browser value instead - same principle Tor Browser uses (every user
presents an identical, unremarkable fingerprint). See `DESIGN.md` "Identity
metadata leaks" for the full requirement and the DNS-resolution check that
was also confirmed safe.

## Status

Core client works: GET/POST/PUT/DELETE, HTTP and HTTPS (OpenSSL, SNI +
hostname verification), redirects, `Content-Length` and chunked response
bodies, optional SOCKS5 proxy. No connection pooling — one TCP(+TLS)
connection per request/redirect hop. No local server/SPA/WebSocket hosting
(`EnvelopeHandler`/`SPAHandler`/`EnvelopeWebSocket` in the Java version) —
see `DESIGN.md` and `TODO.md`.
