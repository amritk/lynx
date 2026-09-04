// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/wayland/wayland_window.h"

#include <algorithm>
#include <string>

#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace lynx {
namespace wayland {

WaylandWindow::WaylandWindow(WaylandApp* app, WaylandWindowDelegate* delegate,
                             uint32_t logical_width, uint32_t logical_height,
                             const std::string& title)
    : app_(app),
      delegate_(delegate),
      logical_width_(logical_width),
      logical_height_(logical_height) {
  surface_ = wl_compositor_create_surface(app_->compositor());
  static const wl_surface_listener kSurfaceListener = {
      OnSurfaceEnter,
      OnSurfaceLeave,
  };
  wl_surface_add_listener(surface_, &kSurfaceListener, this);

  xdg_surface_ = xdg_wm_base_get_xdg_surface(app_->wm_base(), surface_);
  static const xdg_surface_listener kXdgSurfaceListener = {
      OnXdgSurfaceConfigure,
  };
  xdg_surface_add_listener(xdg_surface_, &kXdgSurfaceListener, this);

  toplevel_ = xdg_surface_get_toplevel(xdg_surface_);
  // Every slot must be filled: libwayland calls wl_abort() when an event
  // arrives for a NULL listener entry, so the unused events need real no-op
  // functions rather than being left zero-initialized.
  static const xdg_toplevel_listener kToplevelListener = {
      OnToplevelConfigure,
      OnToplevelClose,
      OnToplevelConfigureBounds,
      OnToplevelWmCapabilities,
  };
  xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);
  xdg_toplevel_set_title(toplevel_, title.c_str());
  xdg_toplevel_set_app_id(toplevel_, "org.lynxjs.lynx_explorer");

  // Ask the compositor to draw the window frame. Compositors that only do
  // client-side decorations (and those without the protocol) simply leave the
  // surface undecorated rather than failing.
  if (app_->decoration_manager()) {
    decoration_ = zxdg_decoration_manager_v1_get_toplevel_decoration(
        app_->decoration_manager(), toplevel_);
    static const zxdg_toplevel_decoration_v1_listener kDecorationListener = {
        OnDecorationConfigure,
    };
    zxdg_toplevel_decoration_v1_add_listener(decoration_, &kDecorationListener,
                                             this);
    zxdg_toplevel_decoration_v1_set_mode(
        decoration_, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }

  app_->RegisterWindow(this);

  // Commit the initial (buffer-less) state and wait for the first configure
  // before attaching any buffer, as required by the xdg-shell protocol.
  wl_surface_commit(surface_);
  // Dispatching inline is safe only because no other thread touches this
  // display yet: the render thread does not exist until the LynxView is built,
  // which happens after this constructor returns.
  while (!configured_) {
    if (wl_display_dispatch(app_->display()) < 0) {
      // The connection died, or the compositor never configured us. Leave the
      // window invalid rather than blocking the process forever.
      return;
    }
  }

  applied_buffer_width_ = static_cast<int32_t>(logical_width_) * scale_;
  applied_buffer_height_ = static_cast<int32_t>(logical_height_) * scale_;
  egl_window_ = wl_egl_window_create(surface_, applied_buffer_width_,
                                     applied_buffer_height_);
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  egl_context_ = eglCreateContext(app_->egl_display(), app_->egl_config(),
                                  EGL_NO_CONTEXT, context_attributes);
  if (egl_context_ == EGL_NO_CONTEXT) {
    return;
  }
  egl_surface_ = eglCreateWindowSurface(
      app_->egl_display(), app_->egl_config(),
      reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);
}

WaylandWindow::~WaylandWindow() {
  app_->UnregisterWindow(this);
  if (egl_surface_ != EGL_NO_SURFACE) {
    eglDestroySurface(app_->egl_display(), egl_surface_);
  }
  if (egl_context_ != EGL_NO_CONTEXT) {
    eglDestroyContext(app_->egl_display(), egl_context_);
  }
  if (egl_window_) {
    wl_egl_window_destroy(egl_window_);
  }
  if (decoration_) {
    zxdg_toplevel_decoration_v1_destroy(decoration_);
  }
  if (toplevel_) {
    xdg_toplevel_destroy(toplevel_);
  }
  if (xdg_surface_) {
    xdg_surface_destroy(xdg_surface_);
  }
  if (surface_) {
    wl_surface_destroy(surface_);
  }
}

bool WaylandWindow::MakeCurrent() {
  // The surface is not resized here. This runs before the renderer says what
  // size it is drawing, so anything chosen now would be the configure's size
  // and not the frame's; ResizeForFrame() does it once that is known.
  return eglMakeCurrent(app_->egl_display(), egl_surface_, egl_surface_,
                        egl_context_) == EGL_TRUE;
}

void WaylandWindow::ResizeForFrame(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(buffer_size_mutex_);
  if (egl_window_ == nullptr) {
    return;
  }
  const int32_t scale = pending_buffer_scale_;
  // A buffer must be an integer multiple of its scale, or the compositor
  // destroys the client with wl_surface.invalid_size. Right after a scale
  // change the engine is still drawing frames sized for the old one, so a
  // frame that does not divide is drawn at the size already applied and the
  // new scale waits for the frame that does -- which is the next one, once
  // OnWindowResized has reached the engine.
  if (width % scale != 0 || height % scale != 0) {
    return;
  }
  if (width == applied_buffer_width_ && height == applied_buffer_height_ &&
      scale == applied_buffer_scale_) {
    return;
  }
  wl_egl_window_resize(egl_window_, width, height, 0, 0);
  if (scale != applied_buffer_scale_) {
    // Same thread as the swap that commits this buffer, so the buffer and its
    // scale always agree. UpdateScale() keeps the scale at 1 when the surface
    // is too old to carry one, so this is always sendable.
    wl_surface_set_buffer_scale(surface_, scale);
    applied_buffer_scale_ = scale;
  }
  applied_buffer_width_ = width;
  applied_buffer_height_ = height;
}

bool WaylandWindow::ClearCurrent() {
  return eglMakeCurrent(app_->egl_display(), EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT) == EGL_TRUE;
}

bool WaylandWindow::SwapBuffers() {
  return eglSwapBuffers(app_->egl_display(), egl_surface_) == EGL_TRUE;
}

// static
void WaylandWindow::OnXdgSurfaceConfigure(void* data, xdg_surface* surface,
                                          uint32_t serial) {
  auto* window = static_cast<WaylandWindow*>(data);
  xdg_surface_ack_configure(surface, serial);
  window->configured_ = true;
  window->ApplyPendingConfigure();
}

// static
void WaylandWindow::OnToplevelConfigure(void* data, xdg_toplevel* toplevel,
                                        int32_t width, int32_t height,
                                        wl_array* states) {
  auto* window = static_cast<WaylandWindow*>(data);
  if (width > 0 && height > 0) {
    window->pending_width_ = static_cast<uint32_t>(width);
    window->pending_height_ = static_cast<uint32_t>(height);
  }
}

// static
void WaylandWindow::OnToplevelClose(void* data, xdg_toplevel* toplevel) {
  auto* window = static_cast<WaylandWindow*>(data);
  window->delegate_->OnWindowCloseRequested();
}

void WaylandWindow::ApplyPendingConfigure() {
  if (pending_width_ == 0 || pending_height_ == 0) {
    return;
  }
  const bool changed =
      pending_width_ != logical_width_ || pending_height_ != logical_height_;
  logical_width_ = pending_width_;
  logical_height_ = pending_height_;
  if (changed && egl_window_) {
    PublishPendingScale();
    delegate_->OnWindowResized(logical_width_, logical_height_, scale_);
  }
}

void WaylandWindow::UpdateScale() {
  int32_t scale = 1;
  for (wl_output* output : entered_outputs_) {
    scale = std::max(scale, app_->GetOutputScale(output));
  }
  if (wl_surface_get_version(surface_) <
      WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
    // The surface cannot carry a scale, so rendering at one would produce a
    // buffer the compositor scales up. Stay at 1:1 instead.
    scale = 1;
  }
  if (scale == scale_) {
    return;
  }
  scale_ = scale;
  // The scale is NOT sent from here. wl_surface.set_buffer_scale is
  // double-buffered state that takes effect on the next commit, and that
  // commit comes from the render thread inside eglSwapBuffers. Sending it here
  // would let a buffer sized for the old scale be committed with the new one;
  // when the logical size is odd, the resulting buffer is not an integer
  // multiple of the scale and the compositor kills the client with
  // wl_surface.invalid_size. Publish it alongside the size instead, so
  // MakeCurrent() applies both before the same commit.
  if (egl_window_) {
    PublishPendingScale();
    delegate_->OnWindowResized(logical_width_, logical_height_, scale_);
  }
}

void WaylandWindow::PublishPendingScale() {
  std::lock_guard<std::mutex> lock(buffer_size_mutex_);
  pending_buffer_scale_ = scale_;
}

void WaylandWindow::OnSurfaceEnter(void* data, wl_surface* surface,
                                   wl_output* output) {
  auto* window = static_cast<WaylandWindow*>(data);
  auto& outputs = window->entered_outputs_;
  if (std::find(outputs.begin(), outputs.end(), output) == outputs.end()) {
    outputs.push_back(output);
  }
  window->UpdateScale();
}

void WaylandWindow::OnSurfaceLeave(void* data, wl_surface* surface,
                                   wl_output* output) {
  auto* window = static_cast<WaylandWindow*>(data);
  auto& outputs = window->entered_outputs_;
  outputs.erase(std::remove(outputs.begin(), outputs.end(), output),
                outputs.end());
  window->UpdateScale();
}

void WaylandWindow::OnOutputRemoved(wl_output* output) {
  auto& outputs = entered_outputs_;
  const size_t before = outputs.size();
  outputs.erase(std::remove(outputs.begin(), outputs.end(), output),
                outputs.end());
  if (outputs.size() != before) {
    UpdateScale();
  }
}

void WaylandWindow::OnOutputScaleChanged(wl_output* output) {
  if (std::find(entered_outputs_.begin(), entered_outputs_.end(), output) !=
      entered_outputs_.end()) {
    UpdateScale();
  }
}

// static
void WaylandWindow::OnToplevelConfigureBounds(void* data,
                                              xdg_toplevel* toplevel,
                                              int32_t width, int32_t height) {
  // Advisory maximum size; this window does not constrain itself to it.
}

// static
void WaylandWindow::OnToplevelWmCapabilities(void* data, xdg_toplevel* toplevel,
                                             wl_array* capabilities) {
  // Which window-menu/maximise/minimise/fullscreen actions the compositor
  // supports. Only relevant once this window draws its own decorations.
}

// static
void WaylandWindow::OnDecorationConfigure(
    void* data, zxdg_toplevel_decoration_v1* decoration, uint32_t mode) {
  // The mode is informational here: the surface geometry the compositor wants
  // still arrives through xdg_toplevel.configure.
}

}  // namespace wayland
}  // namespace lynx
