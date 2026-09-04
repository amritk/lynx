// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_LYNX_RENDERER_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_LYNX_RENDERER_H_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "platform/embedder/public/lynx_windowless_renderer.h"

namespace lynx {

namespace wayland {
class WaylandApp;
class WaylandWindow;
}  // namespace wayland

// Connects the Lynx windowless GL renderer to a Wayland window's EGL surface.
// The GL callbacks are invoked from Lynx's raster thread; the EGL context is
// only ever made current through these callbacks, so it stays confined to
// that thread.
class WaylandLynxRenderer : public pub::LynxWindowlessRenderer {
 public:
  WaylandLynxRenderer(wayland::WaylandApp* app, wayland::WaylandWindow* window);
  ~WaylandLynxRenderer() override;

  // Detaches the window before it is destroyed. Subsequent GL callbacks
  // become no-ops.
  void DetachWindow();

  bool OnGLMakeCurrent() override;
  bool OnGLClearCurrent() override;
  bool OnGLPresent() override;
  uint32_t OnGLCreateFBO(int width, int height) override;
  void* OnGLProcResolver(const char* name) override;

  void OnPostTask(lynx_task_t task, uint64_t interval_nanoseconds) override;

  const char* GetClipboardData() override;
  void SetClipboardData(const char* data) override;
  void ActivateSystemCursor(lynx_cursor_type_e cursor_type,
                            const char* path) override;
  void ShowTextInput(bool show) override;

  // True while the engine has an editable focused and is asking the host for
  // text. Read from the Wayland event loop when a key arrives.
  bool text_input_active() const {
    return text_input_active_.load(std::memory_order_relaxed);
  }

 private:
  wayland::WaylandApp* app_;
  std::mutex window_mutex_;
  wayland::WaylandWindow* window_;
  bool swap_interval_configured_ = false;

  // Written from the engine's UI callbacks, read when translating key events.
  std::atomic<bool> text_input_active_{false};

  // Used only when the compositor offers no wl_data_device_manager, so that
  // copy and paste still work within this process.
  std::mutex clipboard_mutex_;
  std::string clipboard_;
};

}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_LYNX_RENDERER_H_
