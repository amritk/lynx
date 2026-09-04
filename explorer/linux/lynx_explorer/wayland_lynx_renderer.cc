// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/wayland_lynx_renderer.h"

#include <EGL/egl.h>

#include <memory>
#include <string>
#include <utility>

#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_clipboard.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_input.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_window.h"

namespace lynx {

namespace {

const char* CursorNameForType(lynx_cursor_type_e type) {
  switch (type) {
    case kLynxCursorTypeText:
      return "xterm";
    case kLynxCursorTypeClick:
      return "pointer";
    case kLynxCursorTypeGrab:
      return "grab";
    case kLynxCursorTypeGrabbing:
      return "grabbing";
    case kLynxCursorTypeWait:
      return "watch";
    case kLynxCursorTypeProgress:
      return "progress";
    default:
      return "left_ptr";
  }
}

}  // namespace

WaylandLynxRenderer::WaylandLynxRenderer(wayland::WaylandApp* app,
                                         wayland::WaylandWindow* window)
    : pub::LynxWindowlessRenderer(kRendererTypeGLDirect),
      app_(app),
      window_(window) {}

WaylandLynxRenderer::~WaylandLynxRenderer() = default;

void WaylandLynxRenderer::DetachWindow() {
  std::lock_guard<std::mutex> lock(window_mutex_);
  window_ = nullptr;
}

bool WaylandLynxRenderer::OnGLMakeCurrent() {
  std::lock_guard<std::mutex> lock(window_mutex_);
  if (!window_) {
    return false;
  }
  if (!window_->MakeCurrent()) {
    return false;
  }
  if (!swap_interval_configured_) {
    // Do not block the raster thread in eglSwapBuffers waiting for frame
    // callbacks; Lynx paces frames itself.
    eglSwapInterval(app_->egl_display(), 0);
    swap_interval_configured_ = true;
  }
  return true;
}

bool WaylandLynxRenderer::OnGLClearCurrent() {
  std::lock_guard<std::mutex> lock(window_mutex_);
  if (!window_) {
    return false;
  }
  return window_->ClearCurrent();
}

bool WaylandLynxRenderer::OnGLPresent() {
  std::lock_guard<std::mutex> lock(window_mutex_);
  if (!window_) {
    return false;
  }
  return window_->SwapBuffers();
}

uint32_t WaylandLynxRenderer::OnGLCreateFBO(int width, int height) {
  // This is the one place the engine states the size it is rendering *this*
  // frame, and it is asked again for every frame, so it is where the surface
  // has to be sized. Doing it from the last xdg_toplevel.configure instead
  // decouples the two: the engine keeps drawing the old size for at least one
  // frame after a resize, and because this returns FBO 0 the engine has no
  // render target of its own to scale -- Skia wraps the default framebuffer at
  // whatever size it was told and scissors its clear to that -- so the commit
  // carries a larger buffer with only the old region drawn. The remainder is a
  // freshly zeroed buffer with an alpha channel, which shows the desktop
  // through the window until the next frame catches up.
  std::lock_guard<std::mutex> lock(window_mutex_);
  if (window_) {
    window_->ResizeForFrame(width, height);
  }
  // Render straight into the window surface's default framebuffer.
  return 0;
}

void* WaylandLynxRenderer::OnGLProcResolver(const char* name) {
  // Deliberately no dlsym(RTLD_DEFAULT) fallback: the only global-scope
  // provider of gl* here is the libepoxy that liblynx.so links, so the
  // fallback would return epoxy's dispatch stubs instead of the driver entry
  // points this context expects. eglGetProcAddress resolves both core and
  // extension functions on EGL 1.5.
  return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void WaylandLynxRenderer::OnPostTask(lynx_task_t task,
                                     uint64_t interval_nanoseconds) {
  std::weak_ptr<LynxWindowlessRenderer> weak_self = weak_from_this();
  app_->PostDelayedTask(
      [weak_self, task]() {
        if (auto self = weak_self.lock()) {
          self->RunTask(task);
        }
      },
      interval_nanoseconds);
}

const char* WaylandLynxRenderer::GetClipboardData() {
  // The C ABI hands the caller a borrowed pointer that it reads after this
  // returns, so it must not point at storage that another thread can
  // reallocate: neither a concurrent SetClipboardData() nor a concurrent
  // GetClipboardData(). A per-thread copy is owned by exactly one reader.
  static thread_local std::string borrowed;
  wayland::WaylandClipboard* clipboard = app_->clipboard();
  if (clipboard != nullptr && clipboard->IsConnected()) {
    // Already cached, so this does not wait on the client that owns the
    // selection -- see WaylandClipboard.
    borrowed = clipboard->Text();
    return borrowed.c_str();
  }
  {
    std::lock_guard<std::mutex> lock(clipboard_mutex_);
    borrowed = clipboard_;
  }
  return borrowed.c_str();
}

void WaylandLynxRenderer::SetClipboardData(const char* data) {
  const std::string text = data ? data : "";
  {
    std::lock_guard<std::mutex> lock(clipboard_mutex_);
    clipboard_ = text;
  }
  wayland::WaylandClipboard* clipboard = app_->clipboard();
  if (clipboard == nullptr || !clipboard->IsConnected()) {
    return;
  }
  // Claiming the selection is a Wayland request and has to run on the loop
  // thread; the engine calls this from wherever the copy happened. The serial
  // is read there too, so it is the most recent one at the moment the request
  // is actually sent.
  wayland::WaylandApp* app = app_;
  app_->PostTask([app, text]() {
    wayland::WaylandClipboard* target = app->clipboard();
    wayland::WaylandInput* input = app->input();
    if (target == nullptr || input == nullptr) {
      return;
    }
    target->Offer(text, input->last_input_serial());
  });
}

void WaylandLynxRenderer::ShowTextInput(bool show) {
  // The engine calls this when an editable starts or stops editing. There is
  // no on-screen keyboard to raise here; the flag is what tells LynxWindow
  // that keystrokes should also be delivered as text.
  text_input_active_.store(show, std::memory_order_relaxed);
}

void WaylandLynxRenderer::ActivateSystemCursor(lynx_cursor_type_e cursor_type,
                                               const char* path) {
  const char* name = CursorNameForType(cursor_type);
  app_->PostTask([app = app_, name]() {
    if (app->input()) {
      app->input()->SetCursor(name);
    }
  });
}

}  // namespace lynx
