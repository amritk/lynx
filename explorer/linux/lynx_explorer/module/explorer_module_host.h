// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_MODULE_EXPLORER_MODULE_HOST_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_MODULE_EXPLORER_MODULE_HOST_H_

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace lynx {

class LynxWindow;

namespace wayland {
class WaylandApp;
}  // namespace wayland

namespace example {

// Turns a URL as the homepage card writes it into something
// LynxWindow::LoadTemplate() accepts.
//
// The card hands out three shapes: an http(s) URL, a plain file path or
// file:// URL, and the Android/iOS asset scheme "file://lynx?local://<path>"
// that navigateTo() builds for the Showcase and DevTool pages. The last one
// is resolved against resources/ next to the executable, which is where the
// build stages bundles -- the same mapping the desktop JS loader applies to
// script paths (see platform/embedder/resource/js_source_loader_desktop.cc)
// and the macOS explorer applies to templates.
//
// Trailing query parameters (?title=..., &bar_color=...) address a native
// navigation bar this explorer does not have, so they are dropped from local
// paths, as the macOS explorer does. http(s) URLs are passed through
// untouched: there the query belongs to the server.
std::string ResolveBundleUrl(const std::string& url);

// The non-JS half of the JS "ExplorerModule". It owns everything that outlives
// a single JS call -- the window to load into, the settings the Settings page
// toggles, and the small preference store behind saveToLocalStorage() -- so
// that explorer_module.cc stays a thin NAPI wrapper.
//
// Every method is safe to call from the JS thread; work that must happen on
// the UI thread is posted to the Wayland event loop.
class ExplorerModuleHost {
 public:
  static ExplorerModuleHost& GetInstance();

  ExplorerModuleHost(const ExplorerModuleHost&) = delete;
  ExplorerModuleHost& operator=(const ExplorerModuleHost&) = delete;

  // Creates the explorer's top-level window and takes ownership of it, so that
  // it outlives any one page: openSchema() navigates the window it owns rather
  // than building another. Returns nullptr when the window could not be
  // created, or when one is already open -- destroy that one first. Call on
  // the UI thread, before Run().
  LynxWindow* CreateWindow(wayland::WaylandApp* app, uint32_t width,
                           uint32_t height, const std::string& title);
  // Destroys that window. Call on the UI thread, after Run() returns.
  void DestroyWindow();

  // Opens |url|, on the UI thread.
  //
  // The window is kept and navigated in place; only the LynxView inside it is
  // replaced, which LynxWindow::LoadTemplate() handles and documents. The
  // Windows and macOS explorers give every openSchema() its own window and
  // LynxView; this explorer is single-window, so it reuses the one it has.
  //
  // Does nothing when no window exists, which is only true during shutdown.
  void OpenSchema(const std::string& url);

  // Settings the card reads back through getSettingInfo(). They are recorded
  // but not acted on; see explorer_module.cc for why.
  void SetThreadMode(int32_t thread_mode);
  int32_t GetThreadMode() const;
  void SetPresetSizeEnabled(bool enabled);
  bool IsPresetSizeEnabled() const;
  void SetRenderNodeEnabled(bool enabled);
  bool IsRenderNodeEnabled() const;

  // Key/value storage backing saveToLocalStorage(), readFromLocalStorage() and
  // saveThemePreferences(). Persisted under $XDG_CONFIG_HOME/lynx_explorer so
  // it survives a restart, and kept in memory as well so a read never has to
  // touch the disk.
  void SaveToLocalStorage(const std::string& key, const std::string& value);
  bool ReadFromLocalStorage(const std::string& key, std::string* value) const;

 private:
  ExplorerModuleHost() = default;
  ~ExplorerModuleHost() = default;

  void OpenOnUiThread(const std::string& url);
  // Reads the preference file on first use. Callers hold |mutex_|.
  void EnsurePreferencesLoaded() const;
  // Writes the whole store out. Callers hold |mutex_|.
  void WritePreferences() const;

  mutable std::mutex mutex_;
  wayland::WaylandApp* app_ = nullptr;
  // Touched only on the UI thread; |mutex_| guards the pointer itself so
  // OpenSchema() can tell from the JS thread whether there is a window left.
  std::unique_ptr<LynxWindow> window_;

  int32_t thread_mode_ = 0;
  bool preset_size_enabled_ = false;
  bool render_node_enabled_ = false;

  mutable bool preferences_loaded_ = false;
  // Keys this process has changed and not yet written. Only these are applied
  // over the file at save time; everything else is taken from disk, so a value
  // another instance changed after this one started is not reverted by being
  // shadowed by a stale in-memory copy.
  mutable std::set<std::string> dirty_keys_;

  // Reads the on-disk file into |out|, replacing any key it also holds.
  // False when a file exists but could not be read in full.
  bool ReadPreferencesFile(std::map<std::string, std::string>* out) const;

  // Removes preference temporaries whose writing process no longer exists.
  static void RemoveOrphanedTemporaries(const std::string& directory);
  mutable std::map<std::string, std::string> preferences_;
};

}  // namespace example
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_MODULE_EXPLORER_MODULE_HOST_H_
