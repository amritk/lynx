// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/module/explorer_module_host.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "explorer/linux/lynx_explorer/lynx_window.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"

namespace lynx {
namespace example {

namespace {

constexpr char kFileScheme[] = "file://";
constexpr char kLocalScheme[] = "file://lynx?local://";

// Deliberately duplicated from lynx_window.cc rather than shared: that copy is
// file-local there, and this module owns no header that file includes.
std::string GetExecutableDirectoryPath() {
  char path[PATH_MAX] = {0};
  const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
  if (length <= 0) {
    return "";
  }
  std::string full_path(path, static_cast<size_t>(length));
  const size_t separator = full_path.find_last_of('/');
  return separator == std::string::npos ? "" : full_path.substr(0, separator);
}

bool StartsWith(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string Trim(const std::string& value) {
  const size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

// Drops the query string, which on this platform is only ever addressed to a
// navigation bar that does not exist here. Only local targets reach this;
// http(s) URLs are returned before it, so a real query is never cut.
//
// Cutting at the first '?' rather than at the bundle suffix on purpose: the
// showcase menu builds `title=` without percent-encoding it, so a card titled
// with anything containing ".lynx.bundle" would defeat a search for the
// suffix and leave the query attached to the path.
std::string DropQueryAfterBundle(const std::string& path) {
  const size_t query = path.find('?');
  if (query == std::string::npos) {
    return path;
  }
  return path.substr(0, query);
}

// Directory holding the preference file, without a trailing separator. Empty
// when neither XDG_CONFIG_HOME nor HOME is set, in which case preferences live
// only in memory for the life of the process.
std::string GetPreferencesDirectory() {
  const char* xdg_config_home = ::getenv("XDG_CONFIG_HOME");
  if (xdg_config_home != nullptr && xdg_config_home[0] == '/') {
    return std::string(xdg_config_home) + "/lynx_explorer";
  }
  const char* home = ::getenv("HOME");
  if (home != nullptr && home[0] == '/') {
    return std::string(home) + "/.config/lynx_explorer";
  }
  return "";
}

// Escapes the separator and the line break so that a value containing either
// -- the recent-session list is JSON, and a user-typed URL can contain
// anything -- still round-trips through the line-based file below.
std::string Escape(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    switch (character) {
      case '\\':
        escaped.append("\\\\");
        break;
      case '\n':
        escaped.append("\\n");
        break;
      case '\r':
        escaped.append("\\r");
        break;
      case '=':
        escaped.append("\\e");
        break;
      default:
        escaped.push_back(character);
        break;
    }
  }
  return escaped;
}

std::string Unescape(const std::string& value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 == value.size()) {
      unescaped.push_back(value[i]);
      continue;
    }
    switch (value[++i]) {
      case 'n':
        unescaped.push_back('\n');
        break;
      case 'r':
        unescaped.push_back('\r');
        break;
      case 'e':
        unescaped.push_back('=');
        break;
      case '\\':
        unescaped.push_back('\\');
        break;
      default:
        // Not an escape this writer produces; keep both bytes rather than
        // silently dropping one.
        unescaped.push_back('\\');
        unescaped.push_back(value[i]);
        break;
    }
  }
  return unescaped;
}

// mkdir -p for a path that is always absolute here.
bool MakeDirectories(const std::string& path) {
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != '/') {
      continue;
    }
    const std::string prefix = path.substr(0, i);
    if (::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST) {
      return false;
    }
  }
  return true;
}

// True when |resolved| names something worth tearing the current page down
// for. Only a local target can be checked cheaply, and only that check
// matters: an http(s) URL is the user's own instruction, while a local path
// that does not exist is a card pointing at a bundle this build does not
// stage -- the DevTool switch page, for one.
bool IsLoadableTarget(const std::string& resolved) {
  if (StartsWith(resolved, "http://") || StartsWith(resolved, "https://")) {
    return true;
  }
  struct stat info;
  if (::stat(resolved.c_str(), &info) != 0) {
    std::fprintf(stderr, "lynx_explorer: cannot open '%s': %s\n",
                 resolved.c_str(), std::strerror(errno));
    return false;
  }
  if (!S_ISREG(info.st_mode)) {
    std::fprintf(stderr, "lynx_explorer: '%s' is not a regular file\n",
                 resolved.c_str());
    return false;
  }
  return true;
}

}  // namespace

// True when |relative| contains a ".." path component. The leading-slash strip
// below stops local:///etc/passwd from escaping, but says nothing about
// traversal, and "resources/../../../etc/passwd" resolves just as well. The
// component is matched rather than the substring so that a legitimate name
// like "..data" is untouched.
bool HasParentDirectoryComponent(const std::string& relative) {
  size_t start = 0;
  while (start <= relative.size()) {
    size_t end = relative.find('/', start);
    if (end == std::string::npos) {
      end = relative.size();
    }
    if (relative.compare(start, end - start, "..") == 0) {
      return true;
    }
    start = end + 1;
  }
  return false;
}

std::string ResolveBundleUrl(const std::string& url) {
  const std::string trimmed = Trim(url);
  if (StartsWith(trimmed, "http://") || StartsWith(trimmed, "https://")) {
    return trimmed;
  }
  if (StartsWith(trimmed, kLocalScheme)) {
    std::string relative =
        DropQueryAfterBundle(trimmed.substr(sizeof(kLocalScheme) - 1));
    while (!relative.empty() && relative.front() == '/') {
      relative.erase(relative.begin());
    }
    if (HasParentDirectoryComponent(relative)) {
      // The scheme names something inside resources/; a traversal out of it
      // is not a path this explorer is willing to resolve.
      std::fprintf(stderr,
                   "lynx_explorer: refusing '%s': local:// may not contain "
                   "'..'\n",
                   url.c_str());
      return "";
    }
    const std::string directory = GetExecutableDirectoryPath();
    if (directory.empty()) {
      return "resources/" + relative;
    }
    return directory + "/resources/" + relative;
  }
  if (StartsWith(trimmed, kFileScheme)) {
    return DropQueryAfterBundle(trimmed.substr(sizeof(kFileScheme) - 1));
  }
  return DropQueryAfterBundle(trimmed);
}

ExplorerModuleHost& ExplorerModuleHost::GetInstance() {
  static ExplorerModuleHost instance;
  return instance;
}

LynxWindow* ExplorerModuleHost::CreateWindow(wayland::WaylandApp* app,
                                             uint32_t width, uint32_t height,
                                             const std::string& title) {
  // Checked before anything is built. Any previous window must already be
  // gone: assigning over a live one would run ~LynxWindow -- which joins the
  // engine's threads -- while holding this mutex, the deadlock DestroyWindow()
  // goes out of its way to avoid, since a module method on the JS thread can
  // be blocked on this lock inside one of the threads being joined. Checking
  // after construction would also briefly map a second toplevel, which this
  // explorer never wants.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (window_ != nullptr) {
      std::fprintf(stderr,
                   "lynx_explorer: CreateWindow() called with a window already "
                   "open; destroy it first\n");
      return nullptr;
    }
  }
  auto window = std::make_unique<LynxWindow>(app, width, height, title);
  if (!window->IsValid()) {
    return nullptr;
  }
  window->SetQuitOnClose(true);
  std::lock_guard<std::mutex> lock(mutex_);
  app_ = app;
  window_ = std::move(window);
  return window_.get();
}

void ExplorerModuleHost::DestroyWindow() {
  std::unique_ptr<LynxWindow> window;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Moved out and destroyed with the lock released: tearing a LynxView down
    // joins engine threads, and one of them may be inside a module method
    // waiting on this same mutex.
    window = std::move(window_);
  }
}

void ExplorerModuleHost::OpenSchema(const std::string& url) {
  wayland::WaylandApp* app = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    app = app_;
    if (window_ == nullptr || app == nullptr) {
      return;
    }
  }
  const std::string resolved = ResolveBundleUrl(url);
  // The window and the LynxView belong to the UI thread; this runs on the JS
  // thread. A task posted after the loop has stopped is dropped with the
  // queue.
  app->PostTask([resolved]() {
    ExplorerModuleHost::GetInstance().OpenOnUiThread(resolved);
  });
}

void ExplorerModuleHost::OpenOnUiThread(const std::string& url) {
  LynxWindow* window = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    window = window_.get();
  }
  if (window == nullptr) {
    return;
  }
  // Checked before anything is torn down: a target that cannot load would
  // otherwise leave the explorer showing an empty window with the page the
  // user was on already gone.
  if (!IsLoadableTarget(url)) {
    return;
  }
  // Navigate in place. The window is kept -- destroying and re-creating the
  // toplevel made every navigation flash the window away and back, dropped any
  // close request that arrived in the gap, and re-ran the whole EGL and
  // decoration handshake for a page change. LynxWindow replaces the view
  // inside it instead.
  //
  // Called with |mutex_| released: replacing the view joins engine threads,
  // and one of them may be inside a module method waiting on this same mutex.
  // The pointer stays good meanwhile because the window is only ever destroyed
  // from DestroyWindow(), which runs on this thread after the loop stops.
  window->LoadTemplate(url);
}

void ExplorerModuleHost::SetThreadMode(int32_t thread_mode) {
  std::lock_guard<std::mutex> lock(mutex_);
  thread_mode_ = thread_mode;
}

int32_t ExplorerModuleHost::GetThreadMode() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return thread_mode_;
}

void ExplorerModuleHost::SetPresetSizeEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  preset_size_enabled_ = enabled;
}

bool ExplorerModuleHost::IsPresetSizeEnabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return preset_size_enabled_;
}

void ExplorerModuleHost::SetRenderNodeEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  render_node_enabled_ = enabled;
}

bool ExplorerModuleHost::IsRenderNodeEnabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return render_node_enabled_;
}

void ExplorerModuleHost::SaveToLocalStorage(const std::string& key,
                                            const std::string& value) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsurePreferencesLoaded();
  preferences_[key] = value;
  dirty_keys_.insert(key);
  WritePreferences();
}

bool ExplorerModuleHost::ReadFromLocalStorage(const std::string& key,
                                              std::string* value) const {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsurePreferencesLoaded();
  const auto entry = preferences_.find(key);
  if (entry == preferences_.end()) {
    return false;
  }
  *value = entry->second;
  return true;
}

// Reads the file into |preferences_|, merging without overwriting anything
// already in memory. Returns false only when a file exists and could not be
// read in full -- a missing file is an empty store, which is a success.
//
// Merging rather than assigning matters on the second call: a value a card set
// while the file was unreadable was refused for writing but has been readable
// through readFromLocalStorage ever since, so the card believes it took.
// Letting the file win would revert it silently.
bool ExplorerModuleHost::ReadPreferencesFile(
    std::map<std::string, std::string>* out) const {
  const std::string directory = GetPreferencesDirectory();
  if (directory.empty()) {
    return true;
  }
  const std::string path = directory + "/preferences";
  std::ifstream stream(path);
  if (!stream) {
    if (errno == ENOENT) {
      return true;
    }
    std::fprintf(stderr, "lynx_explorer: cannot read '%s': %s\n", path.c_str(),
                 std::strerror(errno));
    return false;
  }
  std::string line;
  while (std::getline(stream, line)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    (*out)[Unescape(line.substr(0, separator))] =
        Unescape(line.substr(separator + 1));
  }
  // getline stopping on an I/O error rather than at end of file leaves a
  // partial map, which has the same consequence as a failed open.
  if (stream.bad()) {
    std::fprintf(stderr, "lynx_explorer: '%s' was only partly read\n",
                 path.c_str());
    return false;
  }
  return true;
}

void ExplorerModuleHost::EnsurePreferencesLoaded() const {
  // Read once for the benefit of readFromLocalStorage. A failure here is not
  // recorded: WritePreferences re-reads immediately before it writes, which is
  // the only moment the answer can cost anything, so there is no persistent
  // failure state to latch, rate-limit or recover from.
  if (preferences_loaded_) {
    return;
  }
  preferences_loaded_ = true;
  ReadPreferencesFile(&preferences_);
}

void ExplorerModuleHost::WritePreferences() const {
  // Start from what is on disk now, then re-apply only the keys this process
  // actually changed. The whole map is about to be serialised over the file,
  // so a key that is on disk but not in memory would be discarded -- and
  // taking the in-memory copy for every key instead would push this process's
  // startup snapshot back over anything another instance changed since,
  // silently reverting it.
  std::map<std::string, std::string> merged;
  if (!ReadPreferencesFile(&merged)) {
    std::fprintf(stderr,
                 "lynx_explorer: not saving preferences, the existing file "
                 "could not be read\n");
    return;
  }
  for (const std::string& key : dirty_keys_) {
    const auto it = preferences_.find(key);
    if (it != preferences_.end()) {
      merged[key] = it->second;
    }
  }
  const std::string directory = GetPreferencesDirectory();
  if (directory.empty() || !MakeDirectories(directory)) {
    return;
  }
  const std::string path = directory + "/preferences";
  // Written to a sibling and renamed so a crash mid-write cannot leave a
  // truncated file behind: rename() within a directory is atomic. This does
  // not survive power loss -- the rename can be journalled while the data
  // blocks are not -- which would need an fsync of the file and its directory.
  const std::string temporary_path =
      path + ".tmp." + std::to_string(static_cast<long>(::getpid()));
  {
    std::ofstream stream(temporary_path, std::ios::trunc);
    if (!stream) {
      std::fprintf(stderr, "lynx_explorer: cannot write '%s': %s\n",
                   temporary_path.c_str(), std::strerror(errno));
      return;
    }
    for (const auto& entry : merged) {
      stream << Escape(entry.first) << '=' << Escape(entry.second) << '\n';
    }
    // Closed explicitly, and checked after: for any realistic preference file
    // nothing has reached the descriptor yet, so a write error surfaces here
    // rather than at the check above. Letting the destructor do it would set
    // badbit on an object nobody can inspect, and the rename below would then
    // promote a short or empty file over a good one.
    stream.close();
    if (!stream) {
      std::fprintf(stderr, "lynx_explorer: failed writing '%s': %s\n",
                   temporary_path.c_str(), std::strerror(errno));
      ::unlink(temporary_path.c_str());
      return;
    }
  }
  if (::rename(temporary_path.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "lynx_explorer: cannot save preferences to '%s': %s\n",
                 path.c_str(), std::strerror(errno));
    ::unlink(temporary_path.c_str());
    return;
  }
  // What is on disk is now exactly |merged|, so adopt it: the in-memory view
  // picks up everything another instance changed, and this process no longer
  // owes those keys.
  preferences_ = std::move(merged);
  dirty_keys_.clear();
  RemoveOrphanedTemporaries(directory);
}

// Clears temporary files left by a process that was killed between writing one
// and renaming it. Nothing else ever reclaims them, and a hard kill during a
// save is not exotic -- the recent-session list is written as the user browses.
// Only files whose owning process is gone are removed, so a second explorer
// running concurrently keeps its own.
void ExplorerModuleHost::RemoveOrphanedTemporaries(
    const std::string& directory) {
  DIR* dir = ::opendir(directory.c_str());
  if (dir == nullptr) {
    return;
  }
  static constexpr char kPrefix[] = "preferences.tmp.";
  while (const dirent* entry = ::readdir(dir)) {
    const std::string name = entry->d_name;
    if (name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) {
      continue;
    }
    const std::string pid_text = name.substr(sizeof(kPrefix) - 1);
    if (pid_text.empty() ||
        pid_text.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    const long pid = ::strtol(pid_text.c_str(), nullptr, 10);
    if (pid <= 0 || pid == static_cast<long>(::getpid())) {
      continue;
    }
    // ESRCH is the only answer that means the writer is definitely gone. EPERM
    // says it is alive under another user, which is a reason to leave it be.
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
      ::unlink((directory + "/" + name).c_str());
    }
  }
  ::closedir(dir);
}

}  // namespace example
}  // namespace lynx
