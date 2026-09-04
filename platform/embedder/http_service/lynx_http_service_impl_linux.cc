// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <unistd.h>

#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform/embedder/http_service/lynx_http_service_impl.h"
#include "third_party/httplib/httplib.h"

namespace lynx {
namespace embedder {

namespace {

// The status code reported for transport/SDK level failures (no HTTP response
// was received), matching the convention used by the macOS and Windows
// services.
constexpr int kSdkErrorStatusCode = 499;

// Largest response body this will buffer. A server can declare or stream an
// arbitrarily large body; accumulating it whole exhausts memory, and on a
// build where allocation failure is not recoverable that takes the process
// down.
constexpr size_t kMaxResponseBytes = 256u * 1024u * 1024u;

// cpp-httplib defaults to a 300s connect timeout, which would leave a JS
// fetch() hanging for minutes on an unreachable host. Use bounded timeouts so
// a stuck transfer always completes the response.
// Matches the bound httplib applies to its own redirect following.
constexpr int kMaxRedirects = 20;
constexpr time_t kConnectionTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 30;
constexpr time_t kWriteTimeoutSeconds = 30;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
// BoringSSL has no usable compiled-in default certificate path on Linux, so
// probe the well-known distribution CA bundles and hand the first one that
// exists to the client. If none is found the client falls back to
// SSL_CTX_set_default_verify_paths().
const char* const kSystemCaCertFiles[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL, CentOS
    "/etc/ssl/ca-bundle.pem",              // openSUSE
    "/etc/pki/tls/cacert.pem",             // OpenELEC
    "/etc/ssl/cert.pem",                   // Alpine, misc.
};

const char* FindSystemCaCertFile() {
  for (const char* path : kSystemCaCertFiles) {
    if (::access(path, R_OK) == 0) {
      return path;
    }
  }
  return nullptr;
}
#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

// Completes the response with a transport-level failure.
void FailResponse(const std::shared_ptr<pub::LynxHttpResponse>& response,
                  const std::string& message) {
  response->SetStatusCode(kSdkErrorStatusCode);
  response->SetStatusText(message.c_str());
}

// Splits "https://host:port/path?query" into the origin ("https://host:port",
// which is what httplib::Client accepts) and the request target
// ("/path?query"). Returns false when |url| has no scheme or no host.
bool SplitUrl(const std::string& url, std::string* origin,
              std::string* target) {
  const size_t scheme_end = url.find("://");
  // An empty scheme ("://host") is not a URL this can act on.
  if (scheme_end == std::string::npos || scheme_end == 0) {
    return false;
  }
  const size_t authority_start = scheme_end + 3;
  // The fragment is client-side only and must never be sent (RFC 7230 5.3).
  const size_t fragment = url.find('#');
  const std::string target_url =
      fragment == std::string::npos ? url : url.substr(0, fragment);
  // The authority ends at the first '/' or '?'. Scanning only for '/' folded a
  // query into the origin, and httplib's client rejects a host containing '?',
  // so "http://host?a=b" failed to resolve at all.
  const size_t path_start = target_url.find_first_of("/?", authority_start);
  if (path_start == std::string::npos) {
    *origin = target_url;
    *target = "/";
  } else {
    *origin = target_url.substr(0, path_start);
    *target = target_url[path_start] == '/'
                  ? target_url.substr(path_start)
                  : "/" + target_url.substr(path_start);
  }
  // Reject "http://" with an empty authority.
  return origin->size() > authority_start;
}

bool IsHttpsUrl(const std::string& url) {
  static constexpr char kHttpsScheme[] = "https://";
  return url.compare(0, sizeof(kHttpsScheme) - 1, kHttpsScheme) == 0;
}

// Resolves a Location value against the origin it was served from. Only the
// absolute and root-relative forms are handled, which is what a redirect in
// practice uses; anything else is refused rather than guessed at.
bool ResolveRedirectTarget(const std::string& origin, const std::string& target,
                           const std::string& location, std::string* next) {
  if (location.empty()) {
    return false;
  }
  if (location.compare(0, 7, "http://") == 0 ||
      location.compare(0, 8, "https://") == 0) {
    *next = location;
    return true;
  }
  if (location.front() == '/') {
    *next = origin + location;
    return true;
  }
  const size_t slash = target.find_last_of('/');
  const std::string base = slash == std::string::npos
                               ? std::string("/")
                               : target.substr(0, slash + 1);
  *next = origin + base + location;
  return true;
}

// Headers that authenticate the caller to the origin the card addressed, and
// so must not travel to a different one.
bool IsCredentialHeader(const std::string& name) {
  static constexpr const char* kCredentialHeaders[] = {
      "authorization", "proxy-authorization", "cookie"};
  std::string lowered = name;
  for (char& c : lowered) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  for (const char* candidate : kCredentialHeaders) {
    if (lowered == candidate) {
      return true;
    }
  }
  return false;
}

bool IsRedirectStatus(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

// True when |method| is an RFC 9110 token, which is the only thing allowed on
// the request line. httplib writes the method into the request line verbatim
// -- unlike the path, which it percent-encodes, and the headers, which it
// filters -- so a method carrying CR or LF emits a second request line. That
// is header injection against any intermediary, and the JS layer does not
// help: lynx-core's Request only upper-cases what it is given.
bool IsHttpToken(const std::string& method) {
  if (method.empty()) {
    return false;
  }
  static const std::string kAllowedSymbols = "!#$%&'*+-.^_`|~";
  for (const char c : method) {
    const bool alphanumeric = (c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (!alphanumeric && kAllowedSymbols.find(c) == std::string::npos) {
      return false;
    }
  }
  return true;
}

// Performs the whole cpp-httplib transfer on a worker thread. |request| and
// |response| are kept alive for the duration; the response completion callback
// fires when the shared_ptr drops (via LynxHttpResponse's destructor, which
// calls lynx_http_response_callback() if it has not already fired).
void PerformRequest(std::shared_ptr<pub::LynxHttpRequest> request,
                    std::shared_ptr<pub::LynxHttpResponse> response) {
  const std::string& url = request->GetUrl();

  std::string origin;
  std::string target;
  if (!SplitUrl(url, &origin, &target)) {
    FailResponse(response, "failed to parse url");
    return;
  }

#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (IsHttpsUrl(url)) {
    FailResponse(response,
                 "https is not supported: this build of cpp-httplib was "
                 "compiled without CPPHTTPLIB_OPENSSL_SUPPORT");
    return;
  }
#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

  // Redirects are followed here rather than by httplib, so that each hop can
  // be checked before the request that carries the card's headers goes out.
  // httplib's own redirect() has no scheme guard -- create_redirect_client()
  // takes a plain client for anything that is not https -- and on a cross-host
  // hop it strips only Host and the two Authorization headers, copying Cookie
  // and any bearer-style header a card set. WinHTTP defaults to
  // DISALLOW_HTTPS_TO_HTTP and NSURLSession is backstopped by ATS, so
  // following blindly would be weaker than either of the other platforms.
  // Validated before it can reach a request line; see IsHttpToken.
  const std::string method =
      request->GetMethod().empty() ? std::string("GET") : request->GetMethod();
  if (!IsHttpToken(method)) {
    FailResponse(response, "invalid http method");
    return;
  }

  std::string current_origin = origin;
  std::string current_target = target;
  std::string current_method = method;
  std::string current_body;
  const std::vector<uint8_t>& body = request->GetBody();
  if (!body.empty()) {
    current_body.assign(reinterpret_cast<const char*>(body.data()),
                        body.size());
  }
  bool cross_origin = false;

  for (int hop = 0;; ++hop) {
    httplib::Client client(current_origin);
    if (!client.is_valid()) {
      FailResponse(response,
                   "failed to create http client for " + current_origin);
      return;
    }
    // Off deliberately; this function does the following. See above.
    client.set_follow_location(false);
    client.set_connection_timeout(kConnectionTimeoutSeconds);
    client.set_read_timeout(kReadTimeoutSeconds);
    client.set_write_timeout(kWriteTimeoutSeconds);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    const char* ca_cert_file = FindSystemCaCertFile();
    if (ca_cert_file != nullptr) {
      client.set_ca_cert_path(ca_cert_file);
    }
#endif  // CPPHTTPLIB_OPENSSL_SUPPORT

    httplib::Request req;
    req.method = current_method;
    req.path = current_target;
    for (const auto& header : request->GetHeaders()) {
      // Once the hop has left the origin the card addressed, anything that
      // authenticates the caller stays behind.
      if (cross_origin && IsCredentialHeader(header.first)) {
        continue;
      }
      req.set_header(header.first, header.second);
    }
    if (!current_body.empty()) {
      req.body = current_body;
    }

    // Accumulate through a receiver so an oversized or endless body is refused
    // while it streams rather than after it has been buffered.
    std::string response_body;
    bool too_large = false;
    req.content_receiver = [&response_body, &too_large](
                               const char* data, size_t length,
                               uint64_t /*offset*/, uint64_t /*total*/) {
      if (response_body.size() + length > kMaxResponseBytes) {
        too_large = true;
        return false;
      }
      response_body.append(data, length);
      return true;
    };

    // Synchronous; connection/DNS/TLS/timeout failures come back as a falsy
    // Result rather than an exception (CPPHTTPLIB_NO_EXCEPTIONS is defined).
    httplib::Result result = client.send(req);
    if (too_large) {
      FailResponse(response, "http response exceeded the maximum size");
      return;
    }
    if (!result) {
      FailResponse(response, "http request failed: " +
                                 httplib::to_string(result.error()));
      return;
    }

    std::string next_url;
    if (IsRedirectStatus(result->status) &&
        ResolveRedirectTarget(current_origin, current_target,
                              result->get_header_value("Location"),
                              &next_url)) {
      if (hop >= kMaxRedirects) {
        FailResponse(response, "too many http redirects");
        return;
      }
      if (IsHttpsUrl(current_origin) && !IsHttpsUrl(next_url)) {
        FailResponse(response,
                     "refusing to follow a redirect from https to http");
        return;
      }
      std::string next_origin;
      std::string next_target;
      if (!SplitUrl(next_url, &next_origin, &next_target)) {
        FailResponse(response, "failed to parse redirect location");
        return;
      }
      if (next_origin != current_origin) {
        cross_origin = true;
      }
      // 303 always becomes a GET, and 301/302 do so for anything that is not
      // already a safe method, which is what every other client does. 307 and
      // 308 exist precisely to preserve the method, so they are left alone.
      if (result->status == 303 ||
          ((result->status == 301 || result->status == 302) &&
           current_method != "GET" && current_method != "HEAD")) {
        current_method = "GET";
        current_body.clear();
      }
      current_origin = next_origin;
      current_target = next_target;
      continue;
    }

    response->SetStatusCode(result->status);
    response->SetStatusText(result->reason.c_str());
    for (const auto& header : result->headers) {
      response->AddHeader(header.first, header.second);
    }

    // SetBody() without a destructor copies the buffer, so the local string
    // may go out of scope right after.
    if (!response_body.empty()) {
      response->SetBody(reinterpret_cast<uint8_t*>(response_body.data()),
                        response_body.size());
    }
    return;
  }
}

}  // namespace

void LynxHttpServiceImpl::Request(
    std::shared_ptr<pub::LynxHttpRequest> request,
    std::shared_ptr<pub::LynxHttpResponse> response) {
  // Request() is invoked on the JS thread; cpp-httplib is used synchronously,
  // so run the transfer on a detached worker thread. The response completion
  // (which marshals the result back to JS) fires when |response| drops.
  std::thread([request = std::move(request),
               response = std::move(response)]() mutable {
    PerformRequest(std::move(request), std::move(response));
  }).detach();
}

}  // namespace embedder
}  // namespace lynx
