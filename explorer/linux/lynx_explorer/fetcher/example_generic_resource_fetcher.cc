// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/fetcher/example_generic_resource_fetcher.h"

#include <stdlib.h>
#include <sys/stat.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "explorer/linux/lynx_explorer/httplib/httplib_client.h"

namespace lynx {
namespace example {

namespace {

constexpr const char kFileScheme[] = "file://";

// Largest local resource this will read into memory.
constexpr uint64_t kMaxResourceBytes = 256ull * 1024ull * 1024ull;

// The directories this fetcher may read from, and the lock that guards them.
// Roots are added on the UI thread and read from detached fetch workers.
std::mutex& AllowedRootsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<std::string>& AllowedRoots() {
  static std::vector<std::string> roots;
  return roots;
}

// Canonicalises |path|, following symlinks, so that neither ".." nor a link
// out of a root can be used to escape it. Empty when the path does not exist.
std::string CanonicalPath(const std::string& path) {
  char* resolved = ::realpath(path.c_str(), nullptr);
  if (resolved == nullptr) {
    return "";
  }
  std::string result(resolved);
  ::free(resolved);
  return result;
}

// True when |path| resolves to a file inside one of the registered roots. The
// comparison is on canonical paths and requires a '/' after the root, so that
// "/tmp/rootsibling" is not accepted for the root "/tmp/root".
bool IsPathAllowed(const std::string& path) {
  const std::string canonical = CanonicalPath(path);
  if (canonical.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(AllowedRootsMutex());
  for (const std::string& root : AllowedRoots()) {
    if (canonical.size() > root.size() &&
        canonical.compare(0, root.size(), root) == 0 &&
        canonical[root.size()] == '/') {
      return true;
    }
  }
  return false;
}

// Refuses anything that is not a regular file. Opening a directory succeeds on
// Linux and then reports a size of LLONG_MAX, and opening a FIFO blocks until a
// writer appears, which would wedge this worker and the shutdown that waits on
// it.
bool ReadFileData(const std::string& path, std::vector<uint8_t>& data) {
  struct stat info;
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return false;
  }
  std::ifstream stream(path, std::ios::ate | std::ios::binary);
  if (!stream) {
    return false;
  }
  const std::streamsize size = stream.tellg();
  // The SIZE_MAX bound alone can never trip on 64-bit, so a sparse or absurdly
  // large file used to reach resize() and abort the process on a failed
  // allocation.
  if (size < 0 || static_cast<uint64_t>(size) > kMaxResourceBytes) {
    return false;
  }
  // Opened with ios::ate to size the file, so rewind before reading.
  stream.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  return static_cast<bool>(
      stream.read(reinterpret_cast<char*>(data.data()), size));
}

}  // namespace

void ExampleGenericResourceFetcher::FetchResource(
    std::shared_ptr<pub::resource::LynxResourceRequest> request,
    std::shared_ptr<pub::resource::LynxResourceResponse> response) {
  auto body = [request, response]() {
    const char* url_chars = request->GetUrl();
    std::string url(url_chars ? url_chars : "");

    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
      std::string content;
      if (!HttplibClient::Get(url, &content)) {
        // Also reported here because the engine does not surface a failed
        // resource fetch through LynxViewClient::OnReceivedError. Without
        // this, an unreachable host, a DNS failure or a 404 on the bundle
        // named on the command line leaves the explorer running with no
        // window and nothing at all on stderr.
        std::fprintf(stderr, "lynx_explorer: could not fetch '%s'\n",
                     url.c_str());
        response->SetCode(-1);
        response->SetErrorMessage("http request failed");
      } else {
        response->SetCode(0);
        // Deliberately not gated on content being non-empty: a 200 with no
        // body is an empty resource, not a failed fetch.
        response->SetData(reinterpret_cast<uint8_t*>(content.data()),
                          content.size());
      }
      response->Complete();
      return;
    }

    std::string path = url;
    if (path.rfind(kFileScheme, 0) == 0) {
      path = path.substr(sizeof(kFileScheme) - 1);
    }
    std::vector<uint8_t> data;
    if (!IsPathAllowed(path)) {
      // A bundle is untrusted code; see AddAllowedRoot in the header.
      std::fprintf(stderr,
                   "lynx_explorer: refusing to read '%s': outside every "
                   "allowed resource directory\n",
                   path.c_str());
      response->SetCode(-1);
      const std::string message =
          "refused to read file outside the allowed "
          "directories: " +
          path;
      response->SetErrorMessage(message.c_str());
      response->Complete();
      return;
    }
    if (ReadFileData(path, data)) {
      response->SetCode(0);
      response->SetData(data.data(), data.size());
    } else {
      response->SetCode(-1);
      const std::string message = "failed to read file: " + path;
      response->SetErrorMessage(message.c_str());
    }
    response->Complete();
  };

  // Not guarded against thread exhaustion: libc++ is built here with
  // _LIBCPP_NO_EXCEPTIONS, so std::thread's constructor calls abort() rather
  // than throwing, and a catch would never run. Bounding concurrent fetches
  // is the only real defence, and this example fetcher does not attempt it.
  std::thread(body).detach();
}

void ExampleGenericResourceFetcher::AddAllowedRoot(
    const std::string& directory) {
  const std::string canonical = CanonicalPath(directory);
  if (canonical.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(AllowedRootsMutex());
  for (const std::string& root : AllowedRoots()) {
    if (root == canonical) {
      return;
    }
  }
  AllowedRoots().push_back(canonical);
}

void ExampleGenericResourceFetcher::FetchResourcePath(
    std::shared_ptr<pub::resource::LynxResourceRequest> request,
    std::shared_ptr<pub::resource::LynxResourceResponse> response) {}

}  // namespace example
}  // namespace lynx
