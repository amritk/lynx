// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_KEY_MAPPING_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_KEY_MAPPING_H_

#include <cstdint>

namespace lynx {
namespace wayland {

// Maps a Linux evdev keycode to the USB HID usage expected in
// lynx_key_event_t::physical. Returns 0 when the key is not mapped.
uint64_t EvdevKeycodeToPhysicalKey(uint32_t evdev_keycode);

// Derives the logical key id expected in lynx_key_event_t::logical from the
// evdev keycode and the code point the key produces with modifier
// transformations ignored (see WaylandInput, which resolves it from the
// unmodified keysym). Passing the Ctrl-transformed character here would map
// every Ctrl+letter to a C0 control and lose the key's identity.
//
// Printable keys map to their (lowercased) Unicode code point; control and
// modifier keys map to fixed ids in the non-printable planes. Returns 0 when
// the key is not mapped.
uint64_t EvdevKeycodeToLogicalKey(uint32_t evdev_keycode,
                                  uint32_t unmodified_code_point);

}  // namespace wayland
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_KEY_MAPPING_H_
