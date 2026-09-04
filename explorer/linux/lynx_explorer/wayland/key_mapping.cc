// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/wayland/key_mapping.h"

#include <linux/input-event-codes.h>

#include <unordered_map>

namespace lynx {
namespace wayland {

namespace {

constexpr uint64_t kHidPlane = 0x00070000;
// Planes used by the logical key ids (matching Flutter's key data layout,
// which the embedder key events follow).
constexpr uint64_t kUnprintablePlane = 0x0100000000;
constexpr uint64_t kModifierPlane = 0x0200000000;

const std::unordered_map<uint32_t, uint64_t>& PhysicalKeyMap() {
  static const std::unordered_map<uint32_t, uint64_t> kMap = {
      {KEY_A, kHidPlane | 0x04},
      {KEY_B, kHidPlane | 0x05},
      {KEY_C, kHidPlane | 0x06},
      {KEY_D, kHidPlane | 0x07},
      {KEY_E, kHidPlane | 0x08},
      {KEY_F, kHidPlane | 0x09},
      {KEY_G, kHidPlane | 0x0a},
      {KEY_H, kHidPlane | 0x0b},
      {KEY_I, kHidPlane | 0x0c},
      {KEY_J, kHidPlane | 0x0d},
      {KEY_K, kHidPlane | 0x0e},
      {KEY_L, kHidPlane | 0x0f},
      {KEY_M, kHidPlane | 0x10},
      {KEY_N, kHidPlane | 0x11},
      {KEY_O, kHidPlane | 0x12},
      {KEY_P, kHidPlane | 0x13},
      {KEY_Q, kHidPlane | 0x14},
      {KEY_R, kHidPlane | 0x15},
      {KEY_S, kHidPlane | 0x16},
      {KEY_T, kHidPlane | 0x17},
      {KEY_U, kHidPlane | 0x18},
      {KEY_V, kHidPlane | 0x19},
      {KEY_W, kHidPlane | 0x1a},
      {KEY_X, kHidPlane | 0x1b},
      {KEY_Y, kHidPlane | 0x1c},
      {KEY_Z, kHidPlane | 0x1d},
      {KEY_1, kHidPlane | 0x1e},
      {KEY_2, kHidPlane | 0x1f},
      {KEY_3, kHidPlane | 0x20},
      {KEY_4, kHidPlane | 0x21},
      {KEY_5, kHidPlane | 0x22},
      {KEY_6, kHidPlane | 0x23},
      {KEY_7, kHidPlane | 0x24},
      {KEY_8, kHidPlane | 0x25},
      {KEY_9, kHidPlane | 0x26},
      {KEY_0, kHidPlane | 0x27},
      {KEY_ENTER, kHidPlane | 0x28},
      {KEY_ESC, kHidPlane | 0x29},
      {KEY_BACKSPACE, kHidPlane | 0x2a},
      {KEY_TAB, kHidPlane | 0x2b},
      {KEY_SPACE, kHidPlane | 0x2c},
      {KEY_MINUS, kHidPlane | 0x2d},
      {KEY_EQUAL, kHidPlane | 0x2e},
      {KEY_LEFTBRACE, kHidPlane | 0x2f},
      {KEY_RIGHTBRACE, kHidPlane | 0x30},
      {KEY_BACKSLASH, kHidPlane | 0x31},
      {KEY_SEMICOLON, kHidPlane | 0x33},
      {KEY_APOSTROPHE, kHidPlane | 0x34},
      {KEY_GRAVE, kHidPlane | 0x35},
      {KEY_COMMA, kHidPlane | 0x36},
      {KEY_DOT, kHidPlane | 0x37},
      {KEY_SLASH, kHidPlane | 0x38},
      {KEY_CAPSLOCK, kHidPlane | 0x39},
      {KEY_F1, kHidPlane | 0x3a},
      {KEY_F2, kHidPlane | 0x3b},
      {KEY_F3, kHidPlane | 0x3c},
      {KEY_F4, kHidPlane | 0x3d},
      {KEY_F5, kHidPlane | 0x3e},
      {KEY_F6, kHidPlane | 0x3f},
      {KEY_F7, kHidPlane | 0x40},
      {KEY_F8, kHidPlane | 0x41},
      {KEY_F9, kHidPlane | 0x42},
      {KEY_F10, kHidPlane | 0x43},
      {KEY_F11, kHidPlane | 0x44},
      {KEY_F12, kHidPlane | 0x45},
      {KEY_INSERT, kHidPlane | 0x49},
      {KEY_HOME, kHidPlane | 0x4a},
      {KEY_PAGEUP, kHidPlane | 0x4b},
      {KEY_DELETE, kHidPlane | 0x4c},
      {KEY_END, kHidPlane | 0x4d},
      {KEY_PAGEDOWN, kHidPlane | 0x4e},
      {KEY_RIGHT, kHidPlane | 0x4f},
      {KEY_LEFT, kHidPlane | 0x50},
      {KEY_DOWN, kHidPlane | 0x51},
      {KEY_UP, kHidPlane | 0x52},
      {KEY_LEFTCTRL, kHidPlane | 0xe0},
      {KEY_LEFTSHIFT, kHidPlane | 0xe1},
      {KEY_LEFTALT, kHidPlane | 0xe2},
      {KEY_LEFTMETA, kHidPlane | 0xe3},
      {KEY_RIGHTCTRL, kHidPlane | 0xe4},
      {KEY_RIGHTSHIFT, kHidPlane | 0xe5},
      {KEY_RIGHTALT, kHidPlane | 0xe6},
      {KEY_RIGHTMETA, kHidPlane | 0xe7},
      // Keypad and the remaining common keys.
      {KEY_SYSRQ, kHidPlane | 0x46},
      {KEY_SCROLLLOCK, kHidPlane | 0x47},
      {KEY_PAUSE, kHidPlane | 0x48},
      {KEY_NUMLOCK, kHidPlane | 0x53},
      {KEY_KPSLASH, kHidPlane | 0x54},
      {KEY_KPASTERISK, kHidPlane | 0x55},
      {KEY_KPMINUS, kHidPlane | 0x56},
      {KEY_KPPLUS, kHidPlane | 0x57},
      {KEY_KPENTER, kHidPlane | 0x58},
      {KEY_KP1, kHidPlane | 0x59},
      {KEY_KP2, kHidPlane | 0x5a},
      {KEY_KP3, kHidPlane | 0x5b},
      {KEY_KP4, kHidPlane | 0x5c},
      {KEY_KP5, kHidPlane | 0x5d},
      {KEY_KP6, kHidPlane | 0x5e},
      {KEY_KP7, kHidPlane | 0x5f},
      {KEY_KP8, kHidPlane | 0x60},
      {KEY_KP9, kHidPlane | 0x61},
      {KEY_KP0, kHidPlane | 0x62},
      {KEY_KPDOT, kHidPlane | 0x63},
      {KEY_102ND, kHidPlane | 0x64},
      {KEY_COMPOSE, kHidPlane | 0x65},
  };
  return kMap;
}

const std::unordered_map<uint32_t, uint64_t>& NonPrintableLogicalKeyMap() {
  static const std::unordered_map<uint32_t, uint64_t> kMap = {
      {KEY_BACKSPACE, kUnprintablePlane | 0x08},
      {KEY_TAB, kUnprintablePlane | 0x09},
      {KEY_ENTER, kUnprintablePlane | 0x0d},
      {KEY_ESC, kUnprintablePlane | 0x1b},
      {KEY_DELETE, kUnprintablePlane | 0x7f},
      {KEY_CAPSLOCK, kUnprintablePlane | 0x104},
      {KEY_DOWN, kUnprintablePlane | 0x301},
      {KEY_LEFT, kUnprintablePlane | 0x302},
      {KEY_RIGHT, kUnprintablePlane | 0x303},
      {KEY_UP, kUnprintablePlane | 0x304},
      {KEY_END, kUnprintablePlane | 0x305},
      {KEY_HOME, kUnprintablePlane | 0x306},
      {KEY_PAGEDOWN, kUnprintablePlane | 0x307},
      {KEY_PAGEUP, kUnprintablePlane | 0x308},
      {KEY_INSERT, kUnprintablePlane | 0x407},
      {KEY_F1, kUnprintablePlane | 0x801},
      {KEY_F2, kUnprintablePlane | 0x802},
      {KEY_F3, kUnprintablePlane | 0x803},
      {KEY_F4, kUnprintablePlane | 0x804},
      {KEY_F5, kUnprintablePlane | 0x805},
      {KEY_F6, kUnprintablePlane | 0x806},
      {KEY_F7, kUnprintablePlane | 0x807},
      {KEY_F8, kUnprintablePlane | 0x808},
      {KEY_F9, kUnprintablePlane | 0x809},
      {KEY_F10, kUnprintablePlane | 0x80a},
      {KEY_F11, kUnprintablePlane | 0x80b},
      {KEY_F12, kUnprintablePlane | 0x80c},
      {KEY_LEFTCTRL, kModifierPlane | 0x100},
      {KEY_RIGHTCTRL, kModifierPlane | 0x101},
      {KEY_LEFTSHIFT, kModifierPlane | 0x102},
      {KEY_RIGHTSHIFT, kModifierPlane | 0x103},
      {KEY_LEFTALT, kModifierPlane | 0x104},
      {KEY_RIGHTALT, kModifierPlane | 0x105},
      {KEY_LEFTMETA, kModifierPlane | 0x106},
      {KEY_RIGHTMETA, kModifierPlane | 0x107},
      // The keypad Enter reports the same logical key as the main Enter. That
      // loses the distinction between them, but leaving it unmapped made the
      // key produce no logical id at all, so it was discarded outright.
      {KEY_KPENTER, kUnprintablePlane | 0x0d},
  };
  return kMap;
}

}  // namespace

uint64_t EvdevKeycodeToPhysicalKey(uint32_t evdev_keycode) {
  const auto& map = PhysicalKeyMap();
  auto it = map.find(evdev_keycode);
  return it != map.end() ? it->second : 0;
}

uint64_t EvdevKeycodeToLogicalKey(uint32_t evdev_keycode,
                                  uint32_t unmodified_code_point) {
  const auto& non_printable = NonPrintableLogicalKeyMap();
  auto it = non_printable.find(evdev_keycode);
  if (it != non_printable.end()) {
    return it->second;
  }
  uint32_t code_point = unmodified_code_point;
  if (code_point >= 0x20 && code_point != 0x7f) {
    // Letters use their lowercase form as the logical id.
    if (code_point >= 'A' && code_point <= 'Z') {
      code_point += 'a' - 'A';
    }
    return code_point;
  }
  return 0;
}

}  // namespace wayland
}  // namespace lynx
