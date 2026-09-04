// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_LYNX_WINDOW_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_LYNX_WINDOW_H_

#include <cstdint>
#include <memory>
#include <string>

#include "explorer/linux/lynx_explorer/wayland/wayland_window.h"
#include "explorer/linux/lynx_explorer/wayland_lynx_renderer.h"
#include "platform/embedder/public/lynx_view.h"

namespace lynx {

namespace wayland {
class WaylandApp;
}  // namespace wayland

// A top-level Wayland window hosting a LynxView rendered through the
// windowless GL renderer. Translates Wayland input into Lynx pointer and key
// events.
class LynxWindow : public wayland::WaylandWindowDelegate {
 public:
  LynxWindow(wayland::WaylandApp* app, uint32_t width, uint32_t height,
             const std::string& title);
  ~LynxWindow() override;

  LynxWindow(const LynxWindow&) = delete;
  LynxWindow& operator=(const LynxWindow&) = delete;

  bool IsValid() const;

  // Loads a Lynx bundle from an http(s) URL, a file:// URL, or a local path.
  //
  // Safe to call again to navigate. The window itself is kept -- the toplevel
  // stays mapped, so a navigation does not flash the window away and back --
  // and only the LynxView inside it is replaced. Replacing it is not
  // optional: a LynxView that has already run a bundle cannot run a second
  // one, because the new bundle's main-thread script is re-evaluated in the
  // context the previous one left behind and the engine rejects it, observed
  // as
  //   load error 110100 ... main-thread.js exception: SyntaxError:
  //   redeclaration of 'e'
  // with nothing drawn.
  //
  // A target that cannot be read leaves the current page on screen: nothing
  // is torn down until the new bundle is known to be loadable.
  // False when nothing was loaded, so a caller that has no page to fall back
  // on -- startup -- can exit rather than run on with an unmapped window.
  bool LoadTemplate(const std::string& url);

  // If true, closing this window quits the application event loop.
  void SetQuitOnClose(bool quit_on_close) { quit_on_close_ = quit_on_close; }

  // The window's current size in logical pixels, which tracks user resizes.
  // Zero before the window is valid.
  uint32_t logical_width() const;
  uint32_t logical_height() const;

  // WaylandWindowDelegate:
  void OnWindowResized(uint32_t logical_width, uint32_t logical_height,
                       int32_t scale) override;
  void OnWindowCloseRequested() override;
  void OnPointerEnter(double x, double y) override;
  void OnPointerLeave() override;
  void OnPointerMotion(double x, double y) override;
  void OnPointerButton(uint32_t button, bool pressed, double x,
                       double y) override;
  void OnPointerAxis(double delta_x, double delta_y, double x, double y,
                     bool precise) override;
  void OnKey(const KeyEvent& event) override;

 private:
  // Builds the renderer and LynxView against the current window. False when
  // the view could not be built.
  bool BuildView();
  // Tears the current view down and builds a fresh one, keeping the window.
  bool RebuildView();

  // Returns true when |url| names something that cannot be loaded, after
  // reporting why.
  bool ReportUnusableUrl(const std::string& url);

  // Delivers |text| to the focused editable as a synthesized text commit.
  void SendTextCommitEvent(const std::string& text);

  void SendPointerEvent(lynx_pointer_phase_e phase,
                        lynx_pointer_signal_kind_e signal_kind, double x,
                        double y, double scroll_delta_x, double scroll_delta_y,
                        bool precise);

  wayland::WaylandApp* app_;

  // Declared before window_: constructing the window blocks dispatching
  // Wayland events, and an xdg_toplevel.close delivered there reaches this
  // object's callbacks, which read these.
  int64_t pressed_buttons_ = 0;

  // Where the pointer was last seen, so that a leave can be reported there.
  // The compositor does not carry a position on wl_pointer.leave, and clay
  // synthesizes a hover at whatever position a remove names -- so reporting
  // the default (0, 0) would hover the page's top-left corner on every exit.
  double pointer_x_ = 0;
  double pointer_y_ = 0;
  bool quit_on_close_ = true;
  // Whether the current view has already been given a bundle, and so has to be
  // replaced before it can be given another.
  bool has_loaded_ = false;

  std::unique_ptr<wayland::WaylandWindow> window_;
  std::shared_ptr<WaylandLynxRenderer> renderer_;
  std::unique_ptr<pub::LynxView> lynx_view_;
  std::shared_ptr<pub::LynxViewClient> client_;
};

}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_LYNX_WINDOW_H_
