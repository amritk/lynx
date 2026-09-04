// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_INPUT_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_INPUT_H_

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include <cstdint>
#include <string>
#include <vector>

namespace lynx {
namespace wayland {

class WaylandApp;
class WaylandWindow;

// Translates wl_seat pointer and keyboard events into WaylandWindowDelegate
// callbacks on the window under focus. Keyboard translation goes through
// xkbcommon using the compositor-provided keymap.
class WaylandInput {
 public:
  explicit WaylandInput(WaylandApp* app);
  ~WaylandInput();

  WaylandInput(const WaylandInput&) = delete;
  WaylandInput& operator=(const WaylandInput&) = delete;

  void BindSeat(wl_registry* registry, uint32_t name, uint32_t version);
  // Releases the seat and everything derived from it, after the compositor
  // has removed the global. Leaves the object able to bind a replacement.
  void UnbindSeat();

  wl_seat* seat() const { return seat_; }

  // Serial of the most recent input event, for requests the compositor
  // validates against one. Zero before any input has arrived.
  uint32_t last_input_serial() const { return last_input_serial_; }
  void OnWindowDestroyed(WaylandWindow* window);

  // Sets the pointer cursor to a named cursor from the default theme (e.g.
  // "left_ptr", "xterm", "pointer"). Only valid while a surface has pointer
  // focus.
  void SetCursor(const char* name);

 private:
  static void OnSeatCapabilities(void* data, wl_seat* seat,
                                 uint32_t capabilities);
  static void OnSeatName(void* data, wl_seat* seat, const char* name);

  // wl_pointer
  static void OnPointerEnter(void* data, wl_pointer* pointer, uint32_t serial,
                             wl_surface* surface, wl_fixed_t x, wl_fixed_t y);
  static void OnPointerLeave(void* data, wl_pointer* pointer, uint32_t serial,
                             wl_surface* surface);
  static void OnPointerMotion(void* data, wl_pointer* pointer, uint32_t time,
                              wl_fixed_t x, wl_fixed_t y);
  static void OnPointerButton(void* data, wl_pointer* pointer, uint32_t serial,
                              uint32_t time, uint32_t button, uint32_t state);
  static void OnPointerAxis(void* data, wl_pointer* pointer, uint32_t time,
                            uint32_t axis, wl_fixed_t value);
  static void OnPointerFrame(void* data, wl_pointer* pointer);
  static void OnPointerAxisSource(void* data, wl_pointer* pointer,
                                  uint32_t axis_source);
  static void OnPointerAxisStop(void* data, wl_pointer* pointer, uint32_t time,
                                uint32_t axis);
  static void OnPointerAxisDiscrete(void* data, wl_pointer* pointer,
                                    uint32_t axis, int32_t discrete);

  // wl_keyboard
  static void OnKeyboardKeymap(void* data, wl_keyboard* keyboard,
                               uint32_t format, int32_t fd, uint32_t size);
  static void OnKeyboardEnter(void* data, wl_keyboard* keyboard,
                              uint32_t serial, wl_surface* surface,
                              wl_array* keys);
  static void OnKeyboardLeave(void* data, wl_keyboard* keyboard,
                              uint32_t serial, wl_surface* surface);
  static void OnKeyboardKey(void* data, wl_keyboard* keyboard, uint32_t serial,
                            uint32_t time, uint32_t key, uint32_t state);
  static void OnKeyboardModifiers(void* data, wl_keyboard* keyboard,
                                  uint32_t serial, uint32_t mods_depressed,
                                  uint32_t mods_latched, uint32_t mods_locked,
                                  uint32_t group);
  static void OnKeyboardRepeatInfo(void* data, wl_keyboard* keyboard,
                                   int32_t rate, int32_t delay);

  void FlushPointerFrame();
  uint32_t CurrentModifiers() const;
  std::string KeyText(uint32_t xkb_keycode) const;
  uint32_t UnmodifiedCodePoint(uint32_t xkb_keycode) const;

  // Tear-down that also notifies the focused window, so losing a device never
  // leaves a button or key logically held.
  void ReleasePointer();
  void ReleaseKeyboard();
  void ReleaseHeldKeys();

  // Key repeat. Repeats are driven off the app event loop; |repeat_epoch_|
  // invalidates timers still in flight when the repeating key changes.
  void StartKeyRepeat(uint32_t evdev_keycode, const std::string& utf8);
  void StopKeyRepeat();
  void ScheduleKeyRepeat(uint64_t delay_ns);
  bool KeyIsRepeatable(uint32_t evdev_keycode) const;

  WaylandApp* app_;
  wl_seat* seat_ = nullptr;
  wl_pointer* pointer_ = nullptr;
  wl_keyboard* keyboard_ = nullptr;

  // Pointer state.
  WaylandWindow* pointer_focus_ = nullptr;
  uint32_t pointer_enter_serial_ = 0;
  // The most recent serial from any input event. A compositor rejects a
  // clipboard selection claimed with a stale one, so the copy path needs
  // whichever serial was seen last rather than a particular kind.
  uint32_t last_input_serial_ = 0;
  double pointer_x_ = 0;
  double pointer_y_ = 0;
  // Pending state accumulated between wl_pointer.frame events.
  bool pending_motion_ = false;
  double pending_axis_x_ = 0;
  double pending_axis_y_ = 0;
  // Wheel notches for this frame, when the compositor reports them. A wheel
  // sends both these and a pixel-ish value on wl_pointer.axis, and only the
  // notch count means the same thing everywhere -- see FlushPointerFrame.
  int32_t pending_axis_discrete_x_ = 0;
  int32_t pending_axis_discrete_y_ = 0;
  bool pending_axis_ = false;
  bool pending_axis_precise_ = false;

  // Keyboard state.
  WaylandWindow* keyboard_focus_ = nullptr;
  // Repeat parameters from wl_keyboard.repeat_info; rate is in keys per
  // second, delay in milliseconds. A rate of 0 disables repeat entirely.
  int32_t repeat_rate_ = 25;
  int32_t repeat_delay_ms_ = 600;
  uint32_t repeating_key_ = 0;
  std::string repeating_utf8_;
  bool repeat_active_ = false;
  uint64_t repeat_epoch_ = 0;
  // Keys currently down, including any reported by wl_keyboard.enter.
  std::vector<uint32_t> pressed_keys_;
  xkb_context* xkb_context_ = nullptr;
  xkb_keymap* xkb_keymap_ = nullptr;
  xkb_state* xkb_state_ = nullptr;

  // Cursor state.
  wl_cursor_theme* cursor_theme_ = nullptr;
  wl_surface* cursor_surface_ = nullptr;
  // Output scale the theme above was loaded for. A cursor theme holds images
  // at one fixed size, so moving to a differently scaled output means loading
  // it again rather than letting the compositor stretch what we have.
  int32_t cursor_scale_ = 0;
  // Cursor currently committed, so that the engine asking for the same one on
  // every pointer motion does not re-do a theme lookup, an attach and a commit
  // each time. Cleared whenever the theme is reloaded.
  std::string applied_cursor_name_;
  // The theme replaced by the last scale change. Destroying a theme frees its
  // wl_buffers, and one of them is still attached to the cursor surface at
  // that moment, so it is kept until another theme's buffer has been attached
  // and committed in its place.
  wl_cursor_theme* retired_cursor_theme_ = nullptr;
};

}  // namespace wayland
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_INPUT_H_
