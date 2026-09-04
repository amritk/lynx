// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_WINDOW_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_WINDOW_H_

#include <EGL/egl.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct xdg_surface;
struct xdg_toplevel;
struct zxdg_toplevel_decoration_v1;

namespace lynx {
namespace wayland {

class WaylandApp;

// Receives window lifecycle and input events. All callbacks are invoked on
// the application event loop thread. Coordinates are surface-local logical
// pixels.
class WaylandWindowDelegate {
 public:
  virtual ~WaylandWindowDelegate() = default;

  virtual void OnWindowResized(uint32_t logical_width, uint32_t logical_height,
                               int32_t scale) = 0;
  virtual void OnWindowCloseRequested() = 0;

  virtual void OnPointerEnter(double x, double y) {}
  virtual void OnPointerLeave() {}
  virtual void OnPointerMotion(double x, double y) {}
  // |button| is a Linux input event code (BTN_LEFT etc.).
  virtual void OnPointerButton(uint32_t button, bool pressed, double x,
                               double y) {}
  virtual void OnPointerAxis(double delta_x, double delta_y, double x, double y,
                             bool precise) {}
  // Modifier bits reported in KeyEvent::modifiers. Alt is Mod1 only: AltGr is
  // Mod5 (ISO_Level3_Shift) and is deliberately not reported here, because on
  // many layouts it is how a user types real characters.
  static constexpr uint32_t kModifierShift = 1 << 0;
  static constexpr uint32_t kModifierControl = 1 << 1;
  static constexpr uint32_t kModifierAlt = 1 << 2;
  static constexpr uint32_t kModifierSuper = 1 << 3;

  struct KeyEvent {
    // Linux evdev keycode.
    uint32_t evdev_keycode = 0;
    bool pressed = false;
    // A synthetic press from the key-repeat timer rather than a new physical
    // press.
    bool is_repeat = false;
    uint32_t modifiers = 0;
    // Text the key produces under the active layout, with modifier
    // transformations applied. Empty when the key produces none.
    std::string text;
    // The code point the key produces ignoring modifier transformations, used
    // to identify which key it is. Ctrl+A yields 'a' here but U+0001 in
    // |text|, so the two must not be conflated.
    uint32_t unmodified_code_point = 0;
  };

  virtual void OnKey(const KeyEvent& event) {}
};

// A top-level xdg-shell window backed by a wl_egl_window. The EGL context is
// created on the loop thread but is only ever made current through
// MakeCurrent(), so a renderer thread may own it exclusively.
class WaylandWindow {
 public:
  WaylandWindow(WaylandApp* app, WaylandWindowDelegate* delegate,
                uint32_t logical_width, uint32_t logical_height,
                const std::string& title);
  ~WaylandWindow();

  WaylandWindow(const WaylandWindow&) = delete;
  WaylandWindow& operator=(const WaylandWindow&) = delete;

  bool IsValid() const { return egl_surface_ != EGL_NO_SURFACE; }

  // Called by WaylandApp when an output this surface may be on goes away or
  // changes scale. Both run on the event loop thread.
  void OnOutputRemoved(wl_output* output);
  void OnOutputScaleChanged(wl_output* output);

  // GL context plumbing, callable from any thread.
  bool MakeCurrent();
  bool ClearCurrent();
  bool SwapBuffers();

  // Sizes the surface to the frame the renderer is about to draw, in buffer
  // pixels. Call from the raster thread before drawing, with the size the
  // engine reports for this frame -- not the size of the last configure. The
  // two differ for at least a frame after every resize, and committing a
  // buffer sized by the configure while the engine drew the older size leaves
  // the remainder undrawn, which reads as a transparent hole.
  void ResizeForFrame(int32_t width, int32_t height);

  wl_surface* surface() const { return surface_; }
  uint32_t logical_width() const { return logical_width_; }
  uint32_t logical_height() const { return logical_height_; }
  int32_t scale() const { return scale_; }

  WaylandWindowDelegate* delegate() const { return delegate_; }

 private:
  static void OnXdgSurfaceConfigure(void* data, xdg_surface* surface,
                                    uint32_t serial);
  static void OnToplevelConfigure(void* data, xdg_toplevel* toplevel,
                                  int32_t width, int32_t height,
                                  wl_array* states);
  static void OnToplevelClose(void* data, xdg_toplevel* toplevel);
  static void OnToplevelConfigureBounds(void* data, xdg_toplevel* toplevel,
                                        int32_t width, int32_t height);
  static void OnToplevelWmCapabilities(void* data, xdg_toplevel* toplevel,
                                       wl_array* capabilities);
  static void OnSurfaceEnter(void* data, wl_surface* surface,
                             wl_output* output);
  static void OnSurfaceLeave(void* data, wl_surface* surface,
                             wl_output* output);
  static void OnDecorationConfigure(void* data,
                                    zxdg_toplevel_decoration_v1* decoration,
                                    uint32_t mode);

  void ApplyPendingConfigure();
  void UpdateScale();
  // Records the buffer size the surface should have. The resize itself is
  // deferred to MakeCurrent() so that it happens on the same thread that
  // swaps, rather than racing it. Call on the event loop thread.
  // Publishes the compositor's requested scale for the raster thread to apply
  // alongside the next frame's size.
  void PublishPendingScale();

  WaylandApp* app_;
  WaylandWindowDelegate* delegate_;

  wl_surface* surface_ = nullptr;
  xdg_surface* xdg_surface_ = nullptr;
  xdg_toplevel* toplevel_ = nullptr;
  zxdg_toplevel_decoration_v1* decoration_ = nullptr;
  std::vector<wl_output*> entered_outputs_;

  wl_egl_window* egl_window_ = nullptr;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;

  uint32_t logical_width_;
  uint32_t logical_height_;
  int32_t scale_ = 1;

  // wl_egl_window is not safe to resize while another thread is drawing to or
  // swapping the surface it backs. The event loop thread only publishes the
  // wanted size here; the render thread applies it in MakeCurrent().
  std::mutex buffer_size_mutex_;
  int32_t applied_buffer_width_ = 0;
  int32_t applied_buffer_height_ = 0;
  int32_t pending_buffer_scale_ = 1;
  int32_t applied_buffer_scale_ = 1;

  bool configured_ = false;
  uint32_t pending_width_ = 0;
  uint32_t pending_height_ = 0;
};

}  // namespace wayland
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_WINDOW_H_
