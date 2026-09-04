// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/wayland/wayland_input.h"

#include <linux/input-event-codes.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_window.h"

namespace lynx {
namespace wayland {

namespace {

// The modifier bits themselves live on WaylandWindowDelegate, because the
// delegate that receives them has to test them too.
// One wheel detent, in logical pixels. Matches the Windows host, which uses
// SPI_GETWHEELSCROLLLINES (default three lines) times 100/3 -- Chromium's
// figure. Wayland gives no equivalent setting, so the default is taken.
constexpr double kLogicalPixelsPerWheelNotch = 100.0;

constexpr uint32_t kModifierShift = WaylandWindowDelegate::kModifierShift;
constexpr uint32_t kModifierControl = WaylandWindowDelegate::kModifierControl;
constexpr uint32_t kModifierAlt = WaylandWindowDelegate::kModifierAlt;
constexpr uint32_t kModifierSuper = WaylandWindowDelegate::kModifierSuper;

}  // namespace

WaylandInput::WaylandInput(WaylandApp* app) : app_(app) {
  xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}

WaylandInput::~WaylandInput() {
  StopKeyRepeat();
  if (cursor_surface_) {
    wl_surface_destroy(cursor_surface_);
  }
  if (cursor_theme_) {
    wl_cursor_theme_destroy(cursor_theme_);
  }
  // After the surface above, so no buffer of its is still attached.
  if (retired_cursor_theme_ != nullptr) {
    wl_cursor_theme_destroy(retired_cursor_theme_);
    retired_cursor_theme_ = nullptr;
  }
  ReleasePointer();
  ReleaseKeyboard();
  if (seat_) {
    if (wl_seat_get_version(seat_) >= WL_SEAT_RELEASE_SINCE_VERSION) {
      wl_seat_release(seat_);
    } else {
      wl_seat_destroy(seat_);
    }
    seat_ = nullptr;
  }
  if (xkb_state_) {
    xkb_state_unref(xkb_state_);
  }
  if (xkb_keymap_) {
    xkb_keymap_unref(xkb_keymap_);
  }
  if (xkb_context_) {
    xkb_context_unref(xkb_context_);
  }
}

void WaylandInput::BindSeat(wl_registry* registry, uint32_t name,
                            uint32_t version) {
  if (seat_) {
    return;
  }
  seat_ = static_cast<wl_seat*>(wl_registry_bind(
      registry, name, &wl_seat_interface, std::min(version, 5u)));
  static const wl_seat_listener kSeatListener = {
      OnSeatCapabilities,
      OnSeatName,
  };
  wl_seat_add_listener(seat_, &kSeatListener, this);
}

void WaylandInput::UnbindSeat() {
  if (seat_ == nullptr) {
    return;
  }
  // Held state cannot outlive the device that reported it.
  StopKeyRepeat();
  ReleaseHeldKeys();
  keyboard_focus_ = nullptr;
  pointer_focus_ = nullptr;
  if (keyboard_ != nullptr) {
    if (wl_keyboard_get_version(keyboard_) >=
        WL_KEYBOARD_RELEASE_SINCE_VERSION) {
      wl_keyboard_release(keyboard_);
    } else {
      wl_keyboard_destroy(keyboard_);
    }
    keyboard_ = nullptr;
  }
  if (pointer_ != nullptr) {
    if (wl_pointer_get_version(pointer_) >= WL_POINTER_RELEASE_SINCE_VERSION) {
      wl_pointer_release(pointer_);
    } else {
      wl_pointer_destroy(pointer_);
    }
    pointer_ = nullptr;
  }
  if (wl_seat_get_version(seat_) >= WL_SEAT_RELEASE_SINCE_VERSION) {
    wl_seat_release(seat_);
  } else {
    wl_seat_destroy(seat_);
  }
  seat_ = nullptr;
}

void WaylandInput::OnWindowDestroyed(WaylandWindow* window) {
  if (pointer_focus_ == window) {
    pointer_focus_ = nullptr;
  }
  if (keyboard_focus_ == window) {
    StopKeyRepeat();
    keyboard_focus_ = nullptr;
  }
}

void WaylandInput::ReleasePointer() {
  if (!pointer_) {
    return;
  }
  // Losing the pointer mid-drag must not leave the page with a button stuck
  // down, so deliver the leave the compositor will now never send.
  if (pointer_focus_) {
    pointer_focus_->delegate()->OnPointerLeave();
    pointer_focus_ = nullptr;
  }
  if (wl_pointer_get_version(pointer_) >= WL_POINTER_RELEASE_SINCE_VERSION) {
    wl_pointer_release(pointer_);
  } else {
    wl_pointer_destroy(pointer_);
  }
  pointer_ = nullptr;
}

void WaylandInput::ReleaseKeyboard() {
  if (!keyboard_) {
    return;
  }
  ReleaseHeldKeys();
  StopKeyRepeat();
  keyboard_focus_ = nullptr;
  if (wl_keyboard_get_version(keyboard_) >= WL_KEYBOARD_RELEASE_SINCE_VERSION) {
    wl_keyboard_release(keyboard_);
  } else {
    wl_keyboard_destroy(keyboard_);
  }
  keyboard_ = nullptr;
}

// Delivers a release for every key still down, so a focus change or a
// disappearing keyboard cannot leave a key logically held forever.
void WaylandInput::ReleaseHeldKeys() {
  const std::vector<uint32_t> held = pressed_keys_;
  pressed_keys_.clear();
  for (uint32_t key : held) {
    // Re-checked each time: delivering a release reaches engine code that can
    // clear the focus.
    if (keyboard_focus_) {
      WaylandWindowDelegate::KeyEvent event;
      event.evdev_keycode = key;
      event.pressed = false;
      event.modifiers = CurrentModifiers();
      event.unmodified_code_point = UnmodifiedCodePoint(key + 8);
      keyboard_focus_->delegate()->OnKey(event);
    }
  }
}

// static
void WaylandInput::OnSeatCapabilities(void* data, wl_seat* seat,
                                      uint32_t capabilities) {
  auto* input = static_cast<WaylandInput*>(data);
  const bool has_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  const bool has_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

  if (has_pointer && !input->pointer_) {
    input->pointer_ = wl_seat_get_pointer(seat);
    static const wl_pointer_listener kPointerListener = {
        OnPointerEnter,      OnPointerLeave,    OnPointerMotion,
        OnPointerButton,     OnPointerAxis,     OnPointerFrame,
        OnPointerAxisSource, OnPointerAxisStop, OnPointerAxisDiscrete,
    };
    wl_pointer_add_listener(input->pointer_, &kPointerListener, input);
  } else if (!has_pointer) {
    input->ReleasePointer();
  }

  if (has_keyboard && !input->keyboard_) {
    input->keyboard_ = wl_seat_get_keyboard(seat);
    static const wl_keyboard_listener kKeyboardListener = {
        OnKeyboardKeymap, OnKeyboardEnter,     OnKeyboardLeave,
        OnKeyboardKey,    OnKeyboardModifiers, OnKeyboardRepeatInfo,
    };
    wl_keyboard_add_listener(input->keyboard_, &kKeyboardListener, input);
  } else if (!has_keyboard) {
    input->ReleaseKeyboard();
  }
}

// static
void WaylandInput::OnSeatName(void* data, wl_seat* seat, const char* name) {}

// static
void WaylandInput::OnPointerEnter(void* data, wl_pointer* pointer,
                                  uint32_t serial, wl_surface* surface,
                                  wl_fixed_t x, wl_fixed_t y) {
  auto* input = static_cast<WaylandInput*>(data);
  input->pointer_focus_ = input->app_->FindWindow(surface);
  input->pointer_enter_serial_ = serial;
  input->last_input_serial_ = serial;
  input->pointer_x_ = wl_fixed_to_double(x);
  input->pointer_y_ = wl_fixed_to_double(y);
  // The cursor association is per-enter: set_cursor has to be sent again with
  // this serial, even for the cursor that was already showing, or the
  // compositor keeps its own. Clearing this defeats the same-cursor guard in
  // SetCursor for exactly this one call.
  input->applied_cursor_name_.clear();
  input->SetCursor("left_ptr");
  if (input->pointer_focus_) {
    input->pointer_focus_->delegate()->OnPointerEnter(input->pointer_x_,
                                                      input->pointer_y_);
  }
}

// static
void WaylandInput::OnPointerLeave(void* data, wl_pointer* pointer,
                                  uint32_t serial, wl_surface* surface) {
  auto* input = static_cast<WaylandInput*>(data);
  if (input->pointer_focus_) {
    input->pointer_focus_->delegate()->OnPointerLeave();
  }
  input->pointer_focus_ = nullptr;
}

// static
void WaylandInput::OnPointerMotion(void* data, wl_pointer* pointer,
                                   uint32_t time, wl_fixed_t x, wl_fixed_t y) {
  auto* input = static_cast<WaylandInput*>(data);
  input->pointer_x_ = wl_fixed_to_double(x);
  input->pointer_y_ = wl_fixed_to_double(y);
  input->pending_motion_ = true;
  if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION) {
    input->FlushPointerFrame();
  }
}

// static
void WaylandInput::OnPointerButton(void* data, wl_pointer* pointer,
                                   uint32_t serial, uint32_t time,
                                   uint32_t button, uint32_t state) {
  auto* input = static_cast<WaylandInput*>(data);
  input->last_input_serial_ = serial;
  if (!input->pointer_focus_) {
    return;
  }
  input->pointer_focus_->delegate()->OnPointerButton(
      button, state == WL_POINTER_BUTTON_STATE_PRESSED, input->pointer_x_,
      input->pointer_y_);
}

// static
void WaylandInput::OnPointerAxis(void* data, wl_pointer* pointer, uint32_t time,
                                 uint32_t axis, wl_fixed_t value) {
  auto* input = static_cast<WaylandInput*>(data);
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    input->pending_axis_y_ += wl_fixed_to_double(value);
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    input->pending_axis_x_ += wl_fixed_to_double(value);
  }
  input->pending_axis_ = true;
  if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION) {
    input->FlushPointerFrame();
  }
}

// static
void WaylandInput::OnPointerFrame(void* data, wl_pointer* pointer) {
  static_cast<WaylandInput*>(data)->FlushPointerFrame();
}

// static
void WaylandInput::OnPointerAxisSource(void* data, wl_pointer* pointer,
                                       uint32_t axis_source) {
  auto* input = static_cast<WaylandInput*>(data);
  input->pending_axis_precise_ =
      axis_source == WL_POINTER_AXIS_SOURCE_FINGER ||
      axis_source == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
}

// static
void WaylandInput::OnPointerAxisStop(void* data, wl_pointer* pointer,
                                     uint32_t time, uint32_t axis) {}

// static
void WaylandInput::OnPointerAxisDiscrete(void* data, wl_pointer* pointer,
                                         uint32_t axis, int32_t discrete) {
  auto* input = static_cast<WaylandInput*>(data);
  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    input->pending_axis_discrete_y_ += discrete;
  } else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
    input->pending_axis_discrete_x_ += discrete;
  }
}

void WaylandInput::FlushPointerFrame() {
  if (pointer_focus_) {
    if (pending_motion_) {
      pointer_focus_->delegate()->OnPointerMotion(pointer_x_, pointer_y_);
    }
    double axis_x = pending_axis_x_;
    double axis_y = pending_axis_y_;
    if (!pending_axis_precise_ &&
        (pending_axis_discrete_x_ != 0 || pending_axis_discrete_y_ != 0)) {
      // A wheel's wl_pointer.axis value is whatever the compositor decided a
      // detent is worth -- wlroots forwards libinput's click angle, 15 by
      // default, while mutter sends 10 -- so passing it through makes the same
      // wheel scroll different distances on different desktops, and about a
      // seventh of what this app scrolls on Windows. The notch count is the
      // only datum that means the same thing everywhere, so convert from that
      // using the same figure the Windows host derives from
      // SPI_GETWHEELSCROLLLINES: three lines of 100/3 logical pixels.
      axis_x = pending_axis_discrete_x_ * kLogicalPixelsPerWheelNotch;
      axis_y = pending_axis_discrete_y_ * kLogicalPixelsPerWheelNotch;
    }
    if (pending_axis_ && (axis_x != 0 || axis_y != 0)) {
      pointer_focus_->delegate()->OnPointerAxis(
          axis_x, axis_y, pointer_x_, pointer_y_, pending_axis_precise_);
    }
  }
  pending_motion_ = false;
  pending_axis_ = false;
  pending_axis_precise_ = false;
  pending_axis_x_ = 0;
  pending_axis_y_ = 0;
  pending_axis_discrete_x_ = 0;
  pending_axis_discrete_y_ = 0;
}

// static
void WaylandInput::OnKeyboardKeymap(void* data, wl_keyboard* keyboard,
                                    uint32_t format, int32_t fd,
                                    uint32_t size) {
  auto* input = static_cast<WaylandInput*>(data);
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !input->xkb_context_) {
    close(fd);
    return;
  }
  void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) {
    close(fd);
    return;
  }
  xkb_keymap* keymap = xkb_keymap_new_from_string(
      input->xkb_context_, static_cast<const char*>(mapped),
      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(mapped, size);
  close(fd);
  if (!keymap) {
    return;
  }
  if (input->xkb_state_) {
    xkb_state_unref(input->xkb_state_);
  }
  if (input->xkb_keymap_) {
    xkb_keymap_unref(input->xkb_keymap_);
  }
  input->xkb_keymap_ = keymap;
  input->xkb_state_ = xkb_state_new(keymap);
}

// static
void WaylandInput::OnKeyboardEnter(void* data, wl_keyboard* keyboard,
                                   uint32_t serial, wl_surface* surface,
                                   wl_array* keys) {
  auto* input = static_cast<WaylandInput*>(data);
  input->last_input_serial_ = serial;
  input->keyboard_focus_ = input->app_->FindWindow(surface);
  input->pressed_keys_.clear();
  if (!keys) {
    return;
  }
  // These keys were already down when focus arrived. Recording them keeps a
  // later unpaired release from desynchronizing the held-key set, which is
  // all this list is for.
  //
  // No press is dispatched for them. wl_keyboard.enter says outright that
  // "clients should not use the list of pressed keys to emulate key-press
  // events" -- the order is unspecified, and the keys are not being pressed
  // now, they were already held. Compositors that send the raw set include
  // the modifiers, so alt-tabbing into the window would deliver Alt *and Tab*
  // as fresh presses into the page, and a Tab keydown is something the engine
  // acts on. The modifier mask is no better: the protocol requires
  // wl_keyboard.modifiers to arrive after this event, so CurrentModifiers()
  // here still reports whatever was current before focus was lost.
  const uint32_t* key = static_cast<const uint32_t*>(keys->data);
  const size_t count = keys->size / sizeof(uint32_t);
  for (size_t i = 0; i < count; ++i) {
    input->pressed_keys_.push_back(key[i]);
  }
}

// static
void WaylandInput::OnKeyboardLeave(void* data, wl_keyboard* keyboard,
                                   uint32_t serial, wl_surface* surface) {
  auto* input = static_cast<WaylandInput*>(data);
  // The keys are physically still down but we stop hearing about them, so
  // neither the repeat nor the held state may outlive the focus.
  input->StopKeyRepeat();
  input->ReleaseHeldKeys();
  input->keyboard_focus_ = nullptr;
  // The modifier mask must not outlive the focus either. Modifiers released
  // while another client had focus are never reported here, so a mask kept
  // across the gap describes a keyboard state that has since changed -- hold
  // Ctrl, switch away, release it, switch back, and every key would still be
  // read as a Ctrl chord. The compositor sends a fresh wl_keyboard.modifiers
  // after the next enter.
  if (input->xkb_state_) {
    xkb_state_update_mask(input->xkb_state_, 0, 0, 0, 0, 0, 0);
  }
}

// static
void WaylandInput::OnKeyboardKey(void* data, wl_keyboard* keyboard,
                                 uint32_t serial, uint32_t time, uint32_t key,
                                 uint32_t state) {
  auto* input = static_cast<WaylandInput*>(data);
  input->last_input_serial_ = serial;
  if (!input->keyboard_focus_ || !input->xkb_state_) {
    return;
  }
  const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
  // Wayland reports evdev keycodes; xkb keycodes are offset by 8.
  const xkb_keycode_t xkb_keycode = key + 8;

  WaylandWindowDelegate::KeyEvent event;
  event.evdev_keycode = key;
  event.pressed = pressed;
  event.is_repeat = false;
  event.modifiers = input->CurrentModifiers();
  if (pressed) {
    event.text = input->KeyText(xkb_keycode);
  }
  event.unmodified_code_point = input->UnmodifiedCodePoint(xkb_keycode);

  auto& held = input->pressed_keys_;
  const auto held_it = std::find(held.begin(), held.end(), key);
  if (pressed) {
    if (held_it == held.end()) {
      held.push_back(key);
    }
  } else if (held_it != held.end()) {
    held.erase(held_it);
  }

  input->keyboard_focus_->delegate()->OnKey(event);

  if (pressed) {
    if (input->KeyIsRepeatable(key)) {
      input->StartKeyRepeat(key, event.text);
    } else {
      input->StopKeyRepeat();
    }
  } else if (input->repeating_key_ == key) {
    input->StopKeyRepeat();
  }
}

// static
void WaylandInput::OnKeyboardModifiers(void* data, wl_keyboard* keyboard,
                                       uint32_t serial, uint32_t mods_depressed,
                                       uint32_t mods_latched,
                                       uint32_t mods_locked, uint32_t group) {
  auto* input = static_cast<WaylandInput*>(data);
  if (input->xkb_state_) {
    xkb_state_update_mask(input->xkb_state_, mods_depressed, mods_latched,
                          mods_locked, 0, 0, group);
  }
}

// static
void WaylandInput::OnKeyboardRepeatInfo(void* data, wl_keyboard* keyboard,
                                        int32_t rate, int32_t delay) {
  auto* input = static_cast<WaylandInput*>(data);
  input->repeat_rate_ = rate;
  input->repeat_delay_ms_ = delay;
  if (rate <= 0) {
    // A zero rate means the compositor wants repeat disabled.
    input->StopKeyRepeat();
  }
}

// Text for a key press, with modifier transformations applied.
std::string WaylandInput::KeyText(uint32_t xkb_keycode) const {
  if (!xkb_state_) {
    return "";
  }
  char buffer[64] = {0};
  // xkb_state_key_get_utf8() has snprintf semantics: it returns the number of
  // bytes REQUIRED, which for a level bound to several keysyms can exceed the
  // buffer. Copying that many bytes out would read past the end, and the
  // keymap comes from the compositor, so the length is not ours to trust.
  const int required =
      xkb_state_key_get_utf8(xkb_state_, xkb_keycode, buffer, sizeof(buffer));
  if (required <= 0) {
    return "";
  }
  if (static_cast<size_t>(required) >= sizeof(buffer)) {
    // Truncating would cut a multi-byte sequence in half and hand invalid
    // UTF-8 to the engine. No key a person types produces this much text, so
    // drop it rather than repair it.
    return "";
  }
  return std::string(buffer, static_cast<size_t>(required));
}

// The code point the key produces with modifier transformations ignored.
// xkb_state_key_get_utf8() applies the control transformation, so Ctrl+A
// reports U+0001 there; the key's identity has to come from the keysym.
uint32_t WaylandInput::UnmodifiedCodePoint(uint32_t xkb_keycode) const {
  if (!xkb_state_ || !xkb_keymap_) {
    return 0;
  }
  // Level 0 of the active layout, not xkb_state_key_get_one_sym: that resolves
  // the shift level, so Shift+1 would report '!' and one physical key would
  // produce two different logical keys. xkb_state_key_get_utf8 is no good
  // either, since it applies the control transformation.
  const xkb_layout_index_t layout =
      xkb_state_key_get_layout(xkb_state_, xkb_keycode);
  const xkb_keysym_t* keysyms = nullptr;
  const int count = xkb_keymap_key_get_syms_by_level(xkb_keymap_, xkb_keycode,
                                                     layout, 0, &keysyms);
  if (count > 0 && keysyms != nullptr) {
    return xkb_keysym_to_utf32(keysyms[0]);
  }
  // No keysym at level 0 (an unmapped keycode, or an invalid layout). There is
  // no character to identify the key by, so report none: the physical key is
  // still carried separately. Falling back to the layout-transformed text here
  // would reintroduce exactly the modifier dependence this function exists to
  // avoid.
  return 0;
}

bool WaylandInput::KeyIsRepeatable(uint32_t evdev_keycode) const {
  if (!xkb_keymap_) {
    return false;
  }
  return xkb_keymap_key_repeats(xkb_keymap_, evdev_keycode + 8) == 1;
}

void WaylandInput::StartKeyRepeat(uint32_t evdev_keycode,
                                  const std::string& utf8) {
  StopKeyRepeat();
  if (repeat_rate_ <= 0 || repeat_delay_ms_ < 0) {
    return;
  }
  repeating_key_ = evdev_keycode;
  repeating_utf8_ = utf8;
  repeat_active_ = true;
  ScheduleKeyRepeat(static_cast<uint64_t>(repeat_delay_ms_) * 1000000ull);
}

void WaylandInput::StopKeyRepeat() {
  repeat_active_ = false;
  repeating_key_ = 0;
  repeating_utf8_.clear();
  // Invalidate any timer already queued on the event loop.
  ++repeat_epoch_;
}

void WaylandInput::ScheduleKeyRepeat(uint64_t delay_ns) {
  const uint64_t epoch = repeat_epoch_;
  app_->PostDelayedTask(
      [this, epoch]() {
        if (!repeat_active_ || epoch != repeat_epoch_ || !keyboard_focus_) {
          return;
        }
        WaylandWindowDelegate::KeyEvent event;
        event.evdev_keycode = repeating_key_;
        event.pressed = true;
        event.is_repeat = true;
        event.modifiers = CurrentModifiers();
        event.text = repeating_utf8_;
        event.unmodified_code_point = UnmodifiedCodePoint(repeating_key_ + 8);
        keyboard_focus_->delegate()->OnKey(event);
        if (repeat_rate_ > 0) {
          ScheduleKeyRepeat(1000000000ull /
                            static_cast<uint64_t>(repeat_rate_));
        }
      },
      delay_ns);
}

uint32_t WaylandInput::CurrentModifiers() const {
  if (!xkb_state_) {
    return 0;
  }
  uint32_t modifiers = 0;
  const auto active = [this](const char* name) {
    return xkb_state_mod_name_is_active(xkb_state_, name,
                                        XKB_STATE_MODS_EFFECTIVE) > 0;
  };
  if (active(XKB_MOD_NAME_SHIFT)) {
    modifiers |= kModifierShift;
  }
  if (active(XKB_MOD_NAME_CTRL)) {
    modifiers |= kModifierControl;
  }
  if (active(XKB_MOD_NAME_ALT)) {
    modifiers |= kModifierAlt;
  }
  if (active(XKB_MOD_NAME_LOGO)) {
    modifiers |= kModifierSuper;
  }
  return modifiers;
}

void WaylandInput::SetCursor(const char* name) {
  if (!pointer_ || !app_->shm()) {
    return;
  }
  // Loaded at the scale of the output the pointer is on, so the cursor is as
  // sharp as the rest of the window instead of a 24px image stretched by the
  // compositor. XCURSOR_THEME and XCURSOR_SIZE are what the rest of the
  // desktop reads, so honour them too rather than always taking the default
  // theme at 24.
  const int32_t scale =
      pointer_focus_ ? std::max(1, pointer_focus_->scale()) : 1;
  if (cursor_theme_ != nullptr && cursor_scale_ != scale) {
    // Retired rather than destroyed: one of its buffers is still attached to
    // the cursor surface, and freeing it now would pull it out from under the
    // compositor.
    //
    // The previous retiree can go: it was replaced at least one scale change
    // ago, so a different theme's buffer has been attached and committed over
    // it since. Without this, flipping between two outputs retains a theme per
    // transition -- around a megabyte of mapped shm each, never reclaimed.
    if (retired_cursor_theme_ != nullptr) {
      wl_cursor_theme_destroy(retired_cursor_theme_);
    }
    retired_cursor_theme_ = cursor_theme_;
    cursor_theme_ = nullptr;
  }
  if (!cursor_theme_) {
    const char* theme = ::getenv("XCURSOR_THEME");
    int size = 24;
    if (const char* size_text = ::getenv("XCURSOR_SIZE")) {
      const int parsed = ::atoi(size_text);
      if (parsed > 0 && parsed <= 512) {
        size = parsed;
      }
    }
    cursor_theme_ = wl_cursor_theme_load(theme, size * scale, app_->shm());
    if (!cursor_theme_) {
      return;
    }
    cursor_scale_ = scale;
    // The images behind it are gone, so nothing is committed any more.
    applied_cursor_name_.clear();
    if (cursor_surface_ == nullptr) {
      cursor_surface_ = wl_compositor_create_surface(app_->compositor());
    }
  }
  // The engine asks for a cursor on every hover, which is every motion event.
  // Re-attaching and committing an identical buffer each time is wasted work
  // on both sides of the socket.
  if (applied_cursor_name_ == name) {
    return;
  }
  wl_cursor* cursor = wl_cursor_theme_get_cursor(cursor_theme_, name);
  if (!cursor && strcmp(name, "left_ptr") != 0) {
    cursor = wl_cursor_theme_get_cursor(cursor_theme_, "left_ptr");
  }
  if (!cursor || cursor->image_count == 0) {
    return;
  }
  wl_cursor_image* image = cursor->images[0];
  wl_buffer* buffer = wl_cursor_image_get_buffer(image);
  if (!buffer) {
    return;
  }
  // The buffer scale has to divide the image, and the image is not necessarily
  // the size that was asked for: Xcursor does not resize anything, it returns
  // the nearest nominal size the theme actually ships. Asking a theme with
  // {24, 32, 48, 64, 96} for 72 gets a 64px image back, and committing that at
  // scale 3 is a protocol violation -- wl_surface.error.invalid_size. wlroots
  // currently only logs "Client bug: submitted a buffer whose size is not
  // divisible by scale" for cursor surfaces, but it enforces it fatally on
  // ordinary surfaces and its comment says the tolerance is temporary.
  //
  // So the declared scale is reduced until it divides the image. The cursor is
  // then larger than requested rather than illegal, which is the better of the
  // two failures. Getting the requested size exactly would mean picking a
  // nominal size that is a multiple of the scale, or driving the surface with
  // wp_viewporter; neither is done here.
  int32_t buffer_scale =
      wl_surface_get_version(cursor_surface_) >= 3 ? cursor_scale_ : 1;
  while (buffer_scale > 1 &&
         (image->width % static_cast<uint32_t>(buffer_scale) != 0 ||
          image->height % static_cast<uint32_t>(buffer_scale) != 0)) {
    --buffer_scale;
  }
  // Sent unconditionally. The scale is sticky surface state, so it has to be
  // re-sent when it goes down as well as up -- only raising it leaves a later
  // 1x buffer being read at the previous 2x, a half-size cursor with its
  // hotspot out by the same factor, and no protocol error because the smaller
  // buffer is still a multiple of the stale scale. Tracking the applied value
  // to skip the odd redundant request is not worth a member that can go stale:
  // this is reached only when the cursor name changes.
  wl_surface_set_buffer_scale(cursor_surface_, buffer_scale);
  wl_pointer_set_cursor(pointer_, pointer_enter_serial_, cursor_surface_,
                        static_cast<int32_t>(image->hotspot_x) / buffer_scale,
                        static_cast<int32_t>(image->hotspot_y) / buffer_scale);
  wl_surface_attach(cursor_surface_, buffer, 0, 0);
  wl_surface_damage(cursor_surface_, 0, 0,
                    static_cast<int32_t>(image->width) / buffer_scale,
                    static_cast<int32_t>(image->height) / buffer_scale);
  wl_surface_commit(cursor_surface_);
  applied_cursor_name_ = name;
}

}  // namespace wayland
}  // namespace lynx
