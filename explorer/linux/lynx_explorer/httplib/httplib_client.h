// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_HTTPLIB_HTTPLIB_CLIENT_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_HTTPLIB_HTTPLIB_CLIENT_H_

#include <cstddef>
#include <string>

namespace lynx {
namespace example {

class HttplibClient {
 public:
  // Largest response this will accumulate. A server can declare or stream an
  // arbitrarily large body, and buffering it whole exhausts memory long before
  // anything useful can be done with it.
  static constexpr size_t kMaxResponseBytes = 256u * 1024u * 1024u;

  // Fetches |url| with a blocking HTTP GET. Returns true and fills |body| on a
  // 2xx response, false on any transport failure, a non-2xx status, or a body
  // over kMaxResponseBytes.
  //
  // Success is reported separately from the body because the two are not the
  // same question: a 200 with Content-Length: 0 is a successful fetch of an
  // empty resource -- an empty chunk of JavaScript or CSS is perfectly legal
  // -- and returning just a string made that indistinguishable from a failed
  // request, so the caller reported it to the engine as a transport error.
  static bool Get(const std::string& url, std::string* body);
};

}  // namespace example
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_HTTPLIB_HTTPLIB_CLIENT_H_
