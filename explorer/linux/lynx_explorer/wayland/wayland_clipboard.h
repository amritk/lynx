// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_CLIPBOARD_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_CLIPBOARD_H_

#include <wayland-client.h>

#include <mutex>
#include <string>
#include <vector>

namespace lynx {
namespace wayland {

class WaylandApp;

// The system clipboard, over wl_data_device.
//
// Reading is eager rather than on demand: whenever the compositor announces a
// new selection, the text is pulled in and cached. The engine's clipboard
// getter is synchronous and returns a borrowed pointer, while a Wayland paste
// is a pipe plus a round trip to whichever client owns the selection, so
// fetching inside the getter would mean blocking the event loop on another
// process at exactly the moment a card is asking for data. Caching moves that
// cost to selection changes, which are rare and not on any critical path.
class WaylandClipboard {
 public:
  explicit WaylandClipboard(WaylandApp* app);
  ~WaylandClipboard();

  WaylandClipboard(const WaylandClipboard&) = delete;
  WaylandClipboard& operator=(const WaylandClipboard&) = delete;

  // Binds wl_data_device_manager from the registry.
  void BindManager(wl_registry* registry, uint32_t name, uint32_t version);
  // Drops the manager and the data device after the compositor has removed
  // the global, so that a later copy does not reach a destroyed resource.
  void UnbindManager();
  // Drops just the data device, for when the seat it came from is removed.
  void UnbindSeat();

  // Attaches to |seat| once both it and the manager exist. Safe to call when
  // either is missing; the clipboard then stays process-local.
  void SetSeat(wl_seat* seat);

  bool IsConnected() const { return device_ != nullptr; }

  // The current selection as UTF-8, empty when there is none or it is not
  // text. Callable from any thread.
  std::string Text() const;

  // Takes ownership of the selection and offers |text| to other clients.
  // |serial| must come from a recent input event; a compositor rejects a
  // selection claimed with a stale one. Call on the Wayland loop thread.
  void Offer(const std::string& text, uint32_t serial);

 private:
  static void OnDataOffer(void* data, wl_data_device* device,
                          wl_data_offer* offer);
  static void OnSelection(void* data, wl_data_device* device,
                          wl_data_offer* offer);
  static void OnEnter(void* data, wl_data_device* device, uint32_t serial,
                      wl_surface* surface, wl_fixed_t x, wl_fixed_t y,
                      wl_data_offer* offer);
  static void OnLeave(void* data, wl_data_device* device);
  static void OnMotion(void* data, wl_data_device* device, uint32_t time,
                       wl_fixed_t x, wl_fixed_t y);
  static void OnDrop(void* data, wl_data_device* device);

  static void OnOfferMimeType(void* data, wl_data_offer* offer,
                              const char* mime_type);
  static void OnOfferSourceActions(void* data, wl_data_offer* offer,
                                   uint32_t source_actions);
  static void OnOfferAction(void* data, wl_data_offer* offer, uint32_t action);

  static void OnSourceTarget(void* data, wl_data_source* source,
                             const char* mime_type);
  static void OnSourceSend(void* data, wl_data_source* source,
                           const char* mime_type, int32_t fd);
  static void OnSourceCancelled(void* data, wl_data_source* source);
  static void OnSourceDndDropPerformed(void* data, wl_data_source* source);
  static void OnSourceDndFinished(void* data, wl_data_source* source);
  static void OnSourceAction(void* data, wl_data_source* source,
                             uint32_t action);

  // Reads |offer| into the cache. Bounded, so a source client that never
  // writes cannot stall the loop.
  void ReadOffer(wl_data_offer* offer, const char* mime_type);

  void DestroyPendingOffer();
  void DestroySource();
  void DestroyRetiredSource();

  WaylandApp* app_;
  wl_data_device_manager* manager_ = nullptr;
  wl_data_device* device_ = nullptr;

  // The offer announced by the most recent data_offer event, still collecting
  // its mime types until the matching selection event arrives.
  wl_data_offer* pending_offer_ = nullptr;
  std::vector<std::string> pending_mime_types_;

  // Our own selection, alive only while the compositor still points at it.
  wl_data_source* source_ = nullptr;
  std::string source_text_;

  // The source the previous Offer() claimed with, kept alive until its own
  // cancelled arrives. wl_data_device.set_selection replaces atomically, so
  // destroying the outgoing source is never required -- and destroying it
  // early both clears the selection (the compositor answers with
  // selection(NULL)) and drops any wl_data_source.send still queued for a
  // peer that has already asked to paste, completing their paste empty. Only
  // one is tracked: a second Offer() before the first is cancelled retires
  // the newer one and drops the older, which is the same bound the
  // compositor puts on how many selections can be outstanding.
  wl_data_source* retired_source_ = nullptr;
  std::string retired_source_text_;

  // True from a successful set_selection until the compositor cancels the
  // source. Nothing in wl_data_offer identifies who produced it, so this
  // remembered bit is the only way to recognise the compositor echoing our
  // own selection back at us; see OnSelection.
  bool owns_selection_ = false;

  mutable std::mutex text_mutex_;
  std::string text_;
};

}  // namespace wayland
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_CLIPBOARD_H_
