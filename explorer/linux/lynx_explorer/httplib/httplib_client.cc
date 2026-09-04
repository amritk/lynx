// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/httplib/httplib_client.h"

#include <unistd.h>

#include <string>
#include <utility>

#include "third_party/httplib/httplib.h"

namespace lynx {
namespace example {

namespace {
// cpp-httplib defaults to a 300s connect timeout. Nothing waits on a fetch any
// more, but an unreachable host would still hold a worker thread and its
// request and response objects for minutes, and a card that issues several
// would accumulate them.
// An absolute ceiling on a single fetch. Nothing else bounds one: the read
// timeout is per-recv, so a slow-drip server is otherwise unbounded. Generous,
// because it caps the whole transfer and a legitimate slow download of a large
// bundle must still finish.
constexpr time_t kMaxTransferMilliseconds = 600 * 1000;
constexpr time_t kConnectionTimeoutSeconds = 10;
constexpr time_t kReadTimeoutSeconds = 20;
constexpr time_t kWriteTimeoutSeconds = 20;

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
const char* const kSystemCaCertFiles[] = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Alpine
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL, CentOS
    "/etc/ssl/ca-bundle.pem",              // openSUSE
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
}  // namespace

bool HttplibClient::Get(const std::string& url, std::string* body_out) {
  std::string::size_type pos = url.find("://");
  // An empty scheme ("://host") is not a URL this can act on.
  if (pos == std::string::npos || pos == 0) {
    return false;
  }
  // The fragment is client-side only and must never be sent (RFC 7230 5.3).
  const std::string::size_type fragment = url.find('#');
  const std::string target_url =
      fragment == std::string::npos ? url : url.substr(0, fragment);
  // The authority ends at the first '/' or '?'; scanning only for '/' folds a
  // query into the host and httplib then fails to parse it.
  const std::string::size_type authority_start = pos + 3;
  pos = target_url.find_first_of("/?", authority_start);
  std::string scheme_host_port;
  std::string path_query;
  if (pos != std::string::npos) {
    scheme_host_port = target_url.substr(0, pos);
    path_query = target_url[pos] == '/' ? target_url.substr(pos)
                                        : "/" + target_url.substr(pos);
  } else {
    scheme_host_port = target_url;
    path_query = "/";
  }
  httplib::Client client(scheme_host_port.c_str());
  if (!client.is_valid()) {
    return false;
  }
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  // BoringSSL has no usable compiled-in certificate path on Linux, so https
  // would fail verification without pointing it at the system bundle.
  const char* ca_cert_file = FindSystemCaCertFile();
  if (ca_cert_file != nullptr) {
    client.set_ca_cert_path(ca_cert_file);
  }
#endif  // CPPHTTPLIB_OPENSSL_SUPPORT
  client.set_follow_location(true);
  // Note: while following redirects, httplib discards the 3xx response's own
  // body without passing it to the receiver, so the response size cap does not
  // apply to it. The body is dropped rather than buffered, so a server that
  // streams a redirect without end stalls this fetch rather than growing
  // memory.
  //
  // The read timeout below is per-recv, not per-transfer: SocketStream::read
  // waits for readability once before each recv, so a server dribbling a byte
  // inside every window keeps the transfer alive indefinitely, and a redirect
  // whose body is discarded never advances redirect_count_ either. The
  // absolute bound is set here instead. set_max_timeout() is what applies it
  // on the Get() path used below; it is deliberately generous, because it caps
  // the whole transfer and a legitimate slow download must still finish.
  client.set_connection_timeout(kConnectionTimeoutSeconds);
  client.set_read_timeout(kReadTimeoutSeconds);
  client.set_write_timeout(kWriteTimeoutSeconds);
  client.set_max_timeout(kMaxTransferMilliseconds);
  // Accumulate through a receiver rather than letting httplib buffer the whole
  // body: that is what bounds memory when a server declares a huge
  // Content-Length or streams chunks without end.
  body_out->clear();
  std::string body;
  bool truncated = false;
  httplib::Result res = client.Get(
      path_query, [&body, &truncated](const char* data, size_t length) {
        if (body.size() + length > kMaxResponseBytes) {
          truncated = true;
          return false;
        }
        body.append(data, length);
        return true;
      });
  if (truncated || res.error() != httplib::Error::Success) {
    return false;
  }
  // Only a 2xx carries a usable body; an error page is not the resource.
  if (res->status < 200 || res->status >= 300) {
    return false;
  }
  // Reached only on a 2xx, so an empty body here is an empty resource, which
  // is a successful fetch.
  *body_out = std::move(body);
  return true;
}

}  // namespace example
}  // namespace lynx
