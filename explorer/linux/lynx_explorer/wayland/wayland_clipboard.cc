// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/wayland/wayland_clipboard.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"

namespace lynx {
namespace wayland {

namespace {

// Offered and accepted, most preferred first. The charset-qualified form is
// what modern toolkits publish; the bare form is still what some older ones
// understand, and both carry UTF-8 in practice.
constexpr const char* kTextMimeTypes[] = {
    "text/plain;charset=utf-8",
    "text/plain",
    "UTF8_STRING",
};

// A selection larger than this is not something a text field wants pasted, and
// the read below is on the event loop.
constexpr size_t kMaxSelectionBytes = 4u * 1024u * 1024u;

// How long the loop will wait for the client that owns the selection to write.
// It is another process and may be busy, wedged, or gone.
constexpr int kReadTimeoutMs = 250;

}  // namespace

WaylandClipboard::WaylandClipboard(WaylandApp* app) : app_(app) {}

WaylandClipboard::~WaylandClipboard() {
  DestroyPendingOffer();
  DestroySource();
  DestroyRetiredSource();
  if (device_ != nullptr) {
    if (wl_data_device_get_version(device_) >=
        WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(device_);
    } else {
      wl_data_device_destroy(device_);
    }
    device_ = nullptr;
  }
  if (manager_ != nullptr) {
    wl_data_device_manager_destroy(manager_);
    manager_ = nullptr;
  }
}

void WaylandClipboard::BindManager(wl_registry* registry, uint32_t name,
                                   uint32_t version) {
  if (manager_ != nullptr) {
    return;
  }
  manager_ = static_cast<wl_data_device_manager*>(
      wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                       std::min(version, 3u)));
}

void WaylandClipboard::UnbindSeat() {
  DestroyPendingOffer();
  DestroySource();
  DestroyRetiredSource();
  owns_selection_ = false;
  if (device_ != nullptr) {
    if (wl_data_device_get_version(device_) >=
        WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(device_);
    } else {
      wl_data_device_destroy(device_);
    }
    device_ = nullptr;
  }
}

void WaylandClipboard::UnbindManager() {
  // The device is derived from the manager, so it goes first.
  UnbindSeat();
  if (manager_ != nullptr) {
    wl_data_device_manager_destroy(manager_);
    manager_ = nullptr;
  }
}

void WaylandClipboard::SetSeat(wl_seat* seat) {
  if (manager_ == nullptr || seat == nullptr || device_ != nullptr) {
    return;
  }
  device_ = wl_data_device_manager_get_data_device(manager_, seat);
  if (device_ == nullptr) {
    return;
  }
  static const wl_data_device_listener kListener = {
      OnDataOffer, OnEnter, OnLeave, OnMotion, OnDrop, OnSelection,
  };
  wl_data_device_add_listener(device_, &kListener, this);
}

std::string WaylandClipboard::Text() const {
  std::lock_guard<std::mutex> lock(text_mutex_);
  return text_;
}

// static
void WaylandClipboard::OnDataOffer(void* data, wl_data_device* device,
                                   wl_data_offer* offer) {
  auto* self = static_cast<WaylandClipboard*>(data);
  // A new offer supersedes one whose selection event never arrived.
  self->DestroyPendingOffer();
  self->pending_offer_ = offer;
  self->pending_mime_types_.clear();
  static const wl_data_offer_listener kListener = {
      OnOfferMimeType,
      OnOfferSourceActions,
      OnOfferAction,
  };
  wl_data_offer_add_listener(offer, &kListener, self);
}

// static
void WaylandClipboard::OnOfferMimeType(void* data, wl_data_offer* offer,
                                       const char* mime_type) {
  auto* self = static_cast<WaylandClipboard*>(data);
  if (offer == self->pending_offer_ && mime_type != nullptr) {
    self->pending_mime_types_.emplace_back(mime_type);
  }
}

// static
void WaylandClipboard::OnOfferSourceActions(void* data, wl_data_offer* offer,
                                            uint32_t source_actions) {}

// static
void WaylandClipboard::OnOfferAction(void* data, wl_data_offer* offer,
                                     uint32_t action) {}

// static
void WaylandClipboard::OnSelection(void* data, wl_data_device* device,
                                   wl_data_offer* offer) {
  auto* self = static_cast<WaylandClipboard*>(data);
  if (offer == nullptr) {
    // The selection was cleared, or its owner exited.
    {
      std::lock_guard<std::mutex> lock(self->text_mutex_);
      self->text_.clear();
    }
    self->DestroyPendingOffer();
    return;
  }
  if (self->owns_selection_) {
    // Our own selection, handed back to us. The compositor sends this event
    // to the focus client whenever a selection is set, with no exclusion for
    // the client that set it, and a copy is driven by a keystroke -- so the
    // focus client is us. Reading it would call wl_data_offer.receive and
    // then block in poll() inside this dispatch callback, waiting for a
    // wl_data_source.send that only this same loop could deliver: a
    // guaranteed stall until the read times out, ending with the empty
    // result overwriting a perfectly good cache.
    //
    // The cache is re-asserted from what we actually published rather than
    // left alone, so it still agrees with the selection even if a compositor
    // orders this event before the cancelled for the source it replaced.
    {
      std::lock_guard<std::mutex> lock(self->text_mutex_);
      self->text_ = self->source_text_;
    }
    self->DestroyPendingOffer();
    return;
  }
  const char* chosen = nullptr;
  for (const char* candidate : kTextMimeTypes) {
    const auto it = std::find(self->pending_mime_types_.begin(),
                              self->pending_mime_types_.end(), candidate);
    if (it != self->pending_mime_types_.end()) {
      chosen = candidate;
      break;
    }
  }
  if (chosen == nullptr) {
    // An image or a file list. Not something a text field can take, and the
    // previous text would be misleading, so the cache is emptied.
    std::lock_guard<std::mutex> lock(self->text_mutex_);
    self->text_.clear();
  } else {
    self->ReadOffer(offer, chosen);
  }
  self->DestroyPendingOffer();
}

void WaylandClipboard::ReadOffer(wl_data_offer* offer, const char* mime_type) {
  int fds[2] = {-1, -1};
  if (::pipe2(fds, O_CLOEXEC) != 0) {
    return;
  }
  wl_data_offer_receive(offer, mime_type, fds[1]);
  ::close(fds[1]);
  // The request has to reach the compositor before anything can be read back;
  // the loop is not dispatching while this runs.
  wl_display_flush(app_->display());

  std::string received;
  char buffer[4096];
  while (received.size() < kMaxSelectionBytes) {
    struct pollfd readable = {};
    readable.fd = fds[0];
    readable.events = POLLIN;
    const int ready = ::poll(&readable, 1, kReadTimeoutMs);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (ready == 0) {
      // The owner never wrote. Whatever it had is not worth stalling for.
      break;
    }
    const ssize_t count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0) {
      break;
    }
    received.append(buffer, static_cast<size_t>(count));
  }
  ::close(fds[0]);

  std::lock_guard<std::mutex> lock(text_mutex_);
  text_ = std::move(received);
}

void WaylandClipboard::Offer(const std::string& text, uint32_t serial) {
  {
    std::lock_guard<std::mutex> lock(text_mutex_);
    text_ = text;
  }
  if (device_ == nullptr || manager_ == nullptr) {
    // No compositor clipboard; the cache above still serves this process.
    return;
  }
  if (serial == 0) {
    // No input event has been seen yet, so there is no serial that a
    // compositor would accept. Claiming with 0 is rejected -- silently, since
    // the protocol gives a rejection no distinct event -- and the attempt
    // would still have disturbed whatever selection is currently set. The
    // cache above keeps copy and paste working inside this process.
    std::fprintf(stderr,
                 "lynx_explorer: not claiming the selection without an input "
                 "serial; the copy is process-local\n");
    return;
  }
  // The outgoing source is retired rather than destroyed: set_selection
  // replaces atomically, and destroying it first would clear the selection
  // and drop a peer's in-flight paste. See the header.
  DestroyRetiredSource();
  retired_source_ = source_;
  retired_source_text_ = std::move(source_text_);
  source_ = nullptr;
  source_text_ = text;
  source_ = wl_data_device_manager_create_data_source(manager_);
  if (source_ == nullptr) {
    return;
  }
  static const wl_data_source_listener kListener = {
      OnSourceTarget,           OnSourceSend,        OnSourceCancelled,
      OnSourceDndDropPerformed, OnSourceDndFinished, OnSourceAction,
  };
  wl_data_source_add_listener(source_, &kListener, this);
  for (const char* mime_type : kTextMimeTypes) {
    wl_data_source_offer(source_, mime_type);
  }
  wl_data_device_set_selection(device_, source_, serial);
  owns_selection_ = true;
}

// static
void WaylandClipboard::OnSourceSend(void* data, wl_data_source* source,
                                    const char* mime_type, int32_t fd) {
  auto* self = static_cast<WaylandClipboard*>(data);
  // A retired source can still be asked to send: a peer may have issued
  // wl_data_offer.receive against the selection we have since replaced.
  static const std::string kNoText;
  const std::string& text = source == self->source_ ? self->source_text_
                            : source == self->retired_source_
                                ? self->retired_source_text_
                                : kNoText;
  size_t written = 0;
  while (written < text.size()) {
    const ssize_t count =
        ::write(fd, text.data() + written, text.size() - written);
    if (count <= 0) {
      // The reader gave up or went away. SIGPIPE is ignored process-wide, so
      // this is EPIPE rather than a signal.
      break;
    }
    written += static_cast<size_t>(count);
  }
  ::close(fd);
}

// static
void WaylandClipboard::OnSourceCancelled(void* data, wl_data_source* source) {
  auto* self = static_cast<WaylandClipboard*>(data);
  // The cached text stays either way: the next selection event replaces it
  // with whatever the new owner offered.
  if (source == self->source_) {
    // The selection we currently hold is gone -- either another client took
    // it, or the compositor refused the claim. The protocol reports both the
    // same way, so this cannot tell them apart; what it can do is stop
    // treating our own offers as self-echoes, which would otherwise make
    // OnSelection skip every future selection.
    self->owns_selection_ = false;
    self->DestroySource();
  } else if (source == self->retired_source_) {
    // Expected: the source this one replaced, released now that the
    // compositor is done with it.
    self->DestroyRetiredSource();
  } else {
    wl_data_source_destroy(source);
  }
}

// static
void WaylandClipboard::OnSourceTarget(void* data, wl_data_source* source,
                                      const char* mime_type) {}
// static
void WaylandClipboard::OnSourceDndDropPerformed(void* data,
                                                wl_data_source* source) {}
// static
void WaylandClipboard::OnSourceDndFinished(void* data, wl_data_source* source) {
}
// static
void WaylandClipboard::OnSourceAction(void* data, wl_data_source* source,
                                      uint32_t action) {}

// Drag and drop is not supported; the handlers exist because libwayland calls
// wl_abort() on a listener with a null slot.
// static
void WaylandClipboard::OnEnter(void* data, wl_data_device* device,
                               uint32_t serial, wl_surface* surface,
                               wl_fixed_t x, wl_fixed_t y,
                               wl_data_offer* offer) {
  auto* self = static_cast<WaylandClipboard*>(data);
  if (offer == nullptr) {
    return;
  }
  // This offer was introduced by a data_offer event first, so it is the same
  // object pending_offer_ holds. Destroying it directly would leave that
  // pointer dangling, and the next DestroyPendingOffer() -- from the next
  // data_offer, from a selection, or from the destructor -- would destroy it
  // a second time and marshal against a dead object id, which the compositor
  // answers by killing the client.
  if (offer == self->pending_offer_) {
    self->DestroyPendingOffer();
  } else {
    wl_data_offer_destroy(offer);
  }
}
// static
void WaylandClipboard::OnLeave(void* data, wl_data_device* device) {}
// static
void WaylandClipboard::OnMotion(void* data, wl_data_device* device,
                                uint32_t time, wl_fixed_t x, wl_fixed_t y) {}
// static
void WaylandClipboard::OnDrop(void* data, wl_data_device* device) {}

void WaylandClipboard::DestroyPendingOffer() {
  if (pending_offer_ != nullptr) {
    wl_data_offer_destroy(pending_offer_);
    pending_offer_ = nullptr;
  }
  pending_mime_types_.clear();
}

void WaylandClipboard::DestroySource() {
  if (source_ != nullptr) {
    wl_data_source_destroy(source_);
    source_ = nullptr;
  }
  source_text_.clear();
}

void WaylandClipboard::DestroyRetiredSource() {
  if (retired_source_ != nullptr) {
    wl_data_source_destroy(retired_source_);
    retired_source_ = nullptr;
  }
  retired_source_text_.clear();
}

}  // namespace wayland
}  // namespace lynx
