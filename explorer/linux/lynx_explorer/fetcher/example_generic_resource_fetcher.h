// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef EXPLORER_LINUX_LYNX_EXPLORER_FETCHER_EXAMPLE_GENERIC_RESOURCE_FETCHER_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_FETCHER_EXAMPLE_GENERIC_RESOURCE_FETCHER_H_

#include <memory>
#include <string>

#include "platform/embedder/public/lynx_generic_resource_fetcher.h"

namespace lynx {
namespace example {

// Resolves http(s) URLs with a blocking GET on a worker thread, and file://
// URLs or plain paths from the local filesystem.
class ExampleGenericResourceFetcher : public pub::LynxGenericResourceFetcher {
 public:
  // Adds a directory this fetcher is allowed to read from. Only files under a
  // registered root are served; everything else is refused.
  //
  // Without this, any URL that is not http(s) is a raw filesystem path, and a
  // card can read (lynx.readScript) and run (lynx.QueryComponent) any file the
  // process can open -- the bytes come back to JS as a string, so it is
  // exfiltration as well as a read. A bundle is untrusted code, so the reader
  // it drives is confined to the directories the *host* chose: the staged
  // resources/ tree, and the directory of a bundle the user explicitly opened.
  // Android's explorer allowlists its schemes and drops everything else; the
  // Windows one is HTTP-only.
  static void AddAllowedRoot(const std::string& directory);

  void FetchResource(
      std::shared_ptr<pub::resource::LynxResourceRequest> request,
      std::shared_ptr<pub::resource::LynxResourceResponse> response) override;

  void FetchResourcePath(
      std::shared_ptr<pub::resource::LynxResourceRequest> request,
      std::shared_ptr<pub::resource::LynxResourceResponse> response) override;

 private:
  // Workers are detached and nothing waits for them. They own everything they
  // touch after this object is gone -- the request and response, both held by
  // shared_ptr -- so outliving the fetcher is safe. The engine's completion
  // callbacks are themselves weak-guarded and bail when their view has been
  // destroyed.
  //
  // There was a bounded wait here. It was not load-bearing: the JS fetch()
  // path in platform/embedder/http_service detaches identical cpp-httplib
  // workers with no bookkeeping at all, so the process already exits with
  // unjoined transfers in flight, and waiting on one of the two populations
  // bought a property the process does not have. It did cost something real,
  // stalling the Wayland event loop for up to five seconds during navigation
  // and shutdown, long enough for a compositor to offer to force-quit.
};

}  // namespace example
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_FETCHER_EXAMPLE_GENERIC_RESOURCE_FETCHER_H_
