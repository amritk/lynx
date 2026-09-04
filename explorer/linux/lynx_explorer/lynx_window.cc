// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/lynx_window.h"

#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "explorer/linux/lynx_explorer/fetcher/example_generic_resource_fetcher.h"
#include "explorer/linux/lynx_explorer/module/explorer_module_host.h"
#include "explorer/linux/lynx_explorer/wayland/key_mapping.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"

namespace lynx {

namespace {

// Largest homepage card this will read into memory.
constexpr uint64_t kMaxResourceBytes = 256ull * 1024ull * 1024ull;

// The theme the card should start in, from what saveThemePreferences() last
// stored. Restricted to a bare word: the value is interpolated into the
// globalProps JSON below, and the only values the card ever writes are
// "light" and "dark", so anything else is a corrupted or hand-edited
// preference file and the default is safer than escaping it.
std::string StoredPreferredTheme() {
  std::string theme;
  if (!example::ExplorerModuleHost::GetInstance().ReadFromLocalStorage(
          "preferredTheme", &theme)) {
    return "light";
  }
  if (theme.empty() || theme.size() > 32) {
    return "light";
  }
  for (const char c : theme) {
    const bool bare = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!bare) {
      return "light";
    }
  }
  return theme;
}

// globalProps is the only channel the card reads its theme from
// (explorer/lib/context.tsx reads lynx.__globalProps.preferredTheme), so a
// preference that is written but never injected here is a preference the user
// cannot actually apply. Both other hosts do this same injection.
std::string BuildGlobalProps() {
  const std::string theme = StoredPreferredTheme();
  return "{\"theme\":\"" + theme + "\",\"preferredTheme\":\"" + theme +
         "\",\"platform\":\"linux\"}";
}

uint64_t NowMicroseconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
         static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

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

// Absolute path to the ICU data staged next to the executable. The windowless
// renderer has no default for Linux (it only knows the Windows and macOS
// layouts) and the binary-icudtl fallback is compiled out in a standalone clay
// build, so without this the engine is handed an empty path.
//
// The homepage card renders identically with the data file removed, so this is
// not load-bearing for it; what needs ICU (collation, break iteration,
// non-Latin shaping) has not been exercised here.
std::string GetICUDataPath() {
  const std::string directory = GetExecutableDirectoryPath();
  if (directory.empty()) {
    return "data/icudtl.dat";
  }
  return directory + "/data/icudtl.dat";
}

// Reads the homepage card staged next to the executable. Returns false when it
// is missing, which is not fatal: the explorer just opens an empty window.
bool LoadHomePage(std::vector<uint8_t>& data) {
  std::string path = GetExecutableDirectoryPath();
  if (!path.empty()) {
    path.append("/");
  }
  path.append("resources/homepage/main.lynx.bundle");
  struct stat info;
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
    return false;
  }
  std::ifstream stream(path, std::ios::ate | std::ios::binary);
  if (!stream) {
    return false;
  }
  const std::streamsize size = stream.tellg();
  if (size < 0 || static_cast<uint64_t>(size) > kMaxResourceBytes) {
    return false;
  }
  // Opened with ios::ate to size the file, so rewind before reading.
  stream.seekg(0, std::ios::beg);
  data.resize(static_cast<size_t>(size));
  return static_cast<bool>(
      stream.read(reinterpret_cast<char*>(data.data()), size));
}

int64_t ButtonToFlag(uint32_t button) {
  switch (button) {
    case BTN_LEFT:
      return kLynxPointerMouseButtonsMousePrimary;
    case BTN_RIGHT:
      return kLynxPointerMouseButtonsMouseSecondary;
    case BTN_MIDDLE:
      return kLynxPointerMouseButtonsMouseMiddle;
    case BTN_BACK:
    case BTN_SIDE:
      return kLynxPointerMouseButtonsMouseBack;
    case BTN_FORWARD:
    case BTN_EXTRA:
      return kLynxPointerMouseButtonsMouseForward;
    default:
      return 0;
  }
}

// Reports engine-side load failures. Without this a bundle that is a valid
// file but not a valid bundle -- a stale build, the wrong file, one from a
// different engine version -- produces a window that never maps and no
// message anywhere.
class ReportingClient : public pub::LynxViewClient {
 public:
  void OnReceivedError(int error_code, const char* message) override {
    std::fprintf(stderr, "lynx_explorer: load error %d: %s\n", error_code,
                 message ? message : "(no message)");
  }
};

}  // namespace

LynxWindow::LynxWindow(wayland::WaylandApp* app, uint32_t width,
                       uint32_t height, const std::string& title)
    : app_(app) {
  window_ =
      std::make_unique<wayland::WaylandWindow>(app, this, width, height, title);
  if (!window_->IsValid()) {
    return;
  }
  BuildView();
}

bool LynxWindow::BuildView() {
  renderer_ = std::make_shared<WaylandLynxRenderer>(app_, window_.get());

  const float logical_width = static_cast<float>(window_->logical_width());
  const float logical_height = static_cast<float>(window_->logical_height());
  const float pixel_ratio = static_cast<float>(window_->scale());

  pub::LynxView::Builder builder;
  builder.SetScreenSize(logical_width, logical_height, pixel_ratio)
      .SetFrame(0, 0, logical_width, logical_height)
      .SetICUDataPath(GetICUDataPath())
      .SetWindowlessRenderer(renderer_)
      .SetGenericResourceFetcher(
          std::make_shared<example::ExampleGenericResourceFetcher>());
  lynx_view_ = builder.Build();
  if (!lynx_view_) {
    return false;
  }
  client_ = std::make_shared<ReportingClient>();
  lynx_view_->AddClient(client_);
  lynx_view_->OnEnterForeground();
  return true;
}

bool LynxWindow::RebuildView() {
  // The view goes first: destroying it joins the engine threads that are
  // still driving the old renderer's GL callbacks, so the renderer must
  // outlive them. Detaching afterwards is what makes any late callback a
  // no-op, exactly as ~LynxWindow does it.
  lynx_view_.reset();
  client_.reset();
  if (renderer_) {
    renderer_->DetachWindow();
    renderer_.reset();
  }
  has_loaded_ = false;
  return BuildView();
}

LynxWindow::~LynxWindow() {
  lynx_view_.reset();
  if (renderer_) {
    renderer_->DetachWindow();
  }
  window_.reset();
}

bool LynxWindow::IsValid() const {
  return window_ && window_->IsValid() && lynx_view_ != nullptr;
}

// Reports an argument that cannot possibly load. Lynx surfaces such failures
// only through the resource response, which the explorer does not render, so
// without this a mistyped path produces a window that never maps and no
// message at all.
bool LynxWindow::ReportUnusableUrl(const std::string& url) {
  if (url.empty() || url.rfind("http://", 0) == 0 ||
      url.rfind("https://", 0) == 0) {
    return false;
  }
  std::string path = url;
  static constexpr char kFileScheme[] = "file://";
  if (path.rfind(kFileScheme, 0) == 0) {
    path = path.substr(sizeof(kFileScheme) - 1);
  }
  struct stat info;
  if (::stat(path.c_str(), &info) != 0) {
    std::fprintf(stderr, "lynx_explorer: cannot open '%s': %s\n", path.c_str(),
                 std::strerror(errno));
    return true;
  }
  if (!S_ISREG(info.st_mode)) {
    std::fprintf(stderr, "lynx_explorer: '%s' is not a regular file\n",
                 path.c_str());
    return true;
  }
  if (static_cast<uint64_t>(info.st_size) > kMaxResourceBytes) {
    std::fprintf(stderr,
                 "lynx_explorer: '%s' is %llu bytes, larger than the %llu byte "
                 "limit\n",
                 path.c_str(), static_cast<unsigned long long>(info.st_size),
                 static_cast<unsigned long long>(kMaxResourceBytes));
    return true;
  }
  return false;
}

uint32_t LynxWindow::logical_width() const {
  return window_ ? window_->logical_width() : 0;
}

uint32_t LynxWindow::logical_height() const {
  return window_ ? window_->logical_height() : 0;
}

bool LynxWindow::LoadTemplate(const std::string& url) {
  if (!window_ || !window_->IsValid()) {
    return false;
  }
  if (ReportUnusableUrl(url)) {
    return false;
  }
  // The staged resources/ tree, and the directory holding a bundle the user
  // named explicitly. These are the only places the resource fetcher will read
  // from; a card cannot widen them. Registered here because this is the one
  // path every load goes through, whether from the command line or from
  // openSchema().
  const std::string executable_directory = GetExecutableDirectoryPath();
  if (!executable_directory.empty()) {
    example::ExampleGenericResourceFetcher::AddAllowedRoot(
        executable_directory + "/resources");
  }
  if (!url.empty() && url.rfind("http://", 0) != 0 &&
      url.rfind("https://", 0) != 0) {
    std::string path = url;
    if (path.rfind("file://", 0) == 0) {
      path = path.substr(sizeof("file://") - 1);
    }
    const size_t separator = path.find_last_of('/');
    if (separator != std::string::npos) {
      example::ExampleGenericResourceFetcher::AddAllowedRoot(
          path.substr(0, separator));
    }
  }

  // Everything that can fail is done before the live view is touched, so a
  // target that turns out to be unreadable leaves the current page on screen
  // rather than emptying the window.
  std::vector<uint8_t> homepage;
  if (url.empty() && !LoadHomePage(homepage)) {
    std::fprintf(stderr,
                 "lynx_explorer: the bundled homepage card is missing from "
                 "resources/homepage/main.lynx.bundle; pass a bundle "
                 "explicitly or rebuild\n");
    return false;
  }
  // A view that has already run a bundle cannot run another; see the header.
  // Only the view is replaced, so the toplevel stays mapped across this.
  if (has_loaded_ && !RebuildView()) {
    std::fprintf(stderr,
                 "lynx_explorer: could not re-create the view for '%s'\n",
                 url.c_str());
    return false;
  }
  if (!lynx_view_) {
    return false;
  }
  auto load_meta = std::make_shared<pub::LynxLoadMeta>();
  load_meta->SetGlobalProps(
      std::make_shared<pub::LynxTemplateData>(BuildGlobalProps()));
  if (url.empty()) {
    load_meta->SetUrl("assets://homepage");
    load_meta->SetBinaryData(std::move(homepage));
  } else {
    load_meta->SetUrl(url);
  }
  lynx_view_->LoadTemplate(load_meta);
  has_loaded_ = true;
  return true;
}

void LynxWindow::OnWindowResized(uint32_t logical_width,
                                 uint32_t logical_height, int32_t scale) {
  if (!lynx_view_) {
    return;
  }
  lynx_view_->UpdateScreenMetrics(static_cast<float>(logical_width),
                                  static_cast<float>(logical_height),
                                  static_cast<float>(scale));
  lynx_view_->SetFrame(0, 0, static_cast<float>(logical_width),
                       static_cast<float>(logical_height));
}

void LynxWindow::OnWindowCloseRequested() {
  if (quit_on_close_) {
    app_->Quit();
  }
}

void LynxWindow::OnPointerEnter(double x, double y) {
  pointer_x_ = x;
  pointer_y_ = y;
  SendPointerEvent(kLynxPointerPhaseAdd, kLynxPointerSignalKindNone, x, y, 0, 0,
                   false);
}

void LynxWindow::OnPointerLeave() {
  // A pointer with pressed buttons keeps sending events elsewhere; reset so
  // the next enter starts from a clean hover state.
  pressed_buttons_ = 0;
  // Reported at the last known position rather than the origin: clay's packet
  // converter synthesizes a hover at a remove's location before removing, so
  // naming (0, 0) would hover the page's top-left corner every time the
  // pointer left the window, and stamp a held button's cancel there too.
  SendPointerEvent(kLynxPointerPhaseRemove, kLynxPointerSignalKindNone,
                   pointer_x_, pointer_y_, 0, 0, false);
}

void LynxWindow::OnPointerMotion(double x, double y) {
  pointer_x_ = x;
  pointer_y_ = y;
  SendPointerEvent(
      pressed_buttons_ ? kLynxPointerPhaseMove : kLynxPointerPhaseHover,
      kLynxPointerSignalKindNone, x, y, 0, 0, false);
}

void LynxWindow::OnPointerButton(uint32_t button, bool pressed, double x,
                                 double y) {
  pointer_x_ = x;
  pointer_y_ = y;
  const int64_t flag = ButtonToFlag(button);
  if (flag == 0) {
    return;
  }
  const int64_t previous_buttons = pressed_buttons_;
  if (pressed) {
    pressed_buttons_ |= flag;
  } else {
    pressed_buttons_ &= ~flag;
  }
  lynx_pointer_phase_e phase;
  if (pressed) {
    phase =
        previous_buttons == 0 ? kLynxPointerPhaseDown : kLynxPointerPhaseMove;
  } else {
    phase = pressed_buttons_ == 0 ? kLynxPointerPhaseUp : kLynxPointerPhaseMove;
  }
  SendPointerEvent(phase, kLynxPointerSignalKindNone, x, y, 0, 0, false);
}

void LynxWindow::OnPointerAxis(double delta_x, double delta_y, double x,
                               double y, bool precise) {
  pointer_x_ = x;
  pointer_y_ = y;
  SendPointerEvent(
      pressed_buttons_ ? kLynxPointerPhaseMove : kLynxPointerPhaseHover,
      kLynxPointerSignalKindScroll, x, y, delta_x, delta_y, precise);
}

void LynxWindow::OnKey(const KeyEvent& key) {
  if (!renderer_) {
    return;
  }
  lynx_key_event_t event = {};
  event.struct_size = sizeof(event);
  event.timestamp = static_cast<double>(NowMicroseconds());
  if (!key.pressed) {
    event.type = kLynxKeyEventTypeUp;
  } else {
    event.type =
        key.is_repeat ? kLynxKeyEventTypeRepeat : kLynxKeyEventTypeDown;
  }
  event.physical = wayland::EvdevKeycodeToPhysicalKey(key.evdev_keycode);
  event.logical = wayland::EvdevKeycodeToLogicalKey(key.evdev_keycode,
                                                    key.unmodified_code_point);
  if (event.physical == 0 && event.logical == 0) {
    return;
  }
  // Whether the key produced a printable character at all. A control byte is
  // not text, so it is not reported as one.
  const bool text_is_printable =
      key.pressed && !key.text.empty() &&
      static_cast<unsigned char>(key.text[0]) >= 0x20 &&
      static_cast<unsigned char>(key.text[0]) != 0x7f;
  event.character = text_is_printable ? key.text.c_str() : nullptr;
  event.synthesized = false;
  renderer_->SendKeyEvent(&event);

  // Committing is a stricter test than reporting: a command modifier means the
  // keystroke is a shortcut, not typing. The printable test above does not
  // cover that, because it only catches what xkb folds into a control code --
  // Ctrl with a letter and a little punctuation. Alt+a and Ctrl+1 both still
  // arrive as plain "a" and "1".
  //
  // Alt is the one that actually leaked: clay tracks Ctrl and Meta itself and
  // swallows their combinations before the text path, but it has no Alt case,
  // so Alt+letter reached the editable and was typed. Ctrl and Super are
  // listed anyway rather than relying on clay to keep doing that.
  //
  // Alt here is Mod1 only. AltGr is Mod5, and on many layouts it is how real
  // characters are typed, so it must not disqualify anything -- with the
  // caveat that a layout option putting level-3 on Mod1 would defeat this.
  const bool has_command_modifier =
      (key.modifiers & (kModifierControl | kModifierAlt | kModifierSuper)) != 0;
  if (text_is_printable && !has_command_modifier &&
      renderer_->text_input_active()) {
    SendTextCommitEvent(key.text);
  }
}

void LynxWindow::SendTextCommitEvent(const std::string& text) {
  // A raw key event never inserts text on this path. clay's editable only
  // treats a key's character as input when the event is flagged synthesized
  // (see EditableView::HandleSynthesizedKeyEvent in
  // clay/ui/component/editable/editable_view.cc); everywhere else it assumes
  // a platform IME will deliver the text separately, and the windowless
  // renderer has no IME hook -- LynxUIRendererWindowless::RegisterIMEHandler
  // is an empty override. So the host has to play the part of the IME and
  // commit the text itself, exactly as the macOS windowed host does when
  // NSTextInputClient hands it insertText:.
  //
  // Physical and logical stay zero: this is a text commit, not a key press.
  // The real press was already reported above, and reusing its ids here would
  // make the editable route the commit through that key's own case in the
  // switch (Space and Enter both have one) instead of the text path.
  lynx_key_event_t commit = {};
  commit.struct_size = sizeof(commit);
  commit.timestamp = static_cast<double>(NowMicroseconds());
  // Always a down event: the commit path ignores repeats, and auto-repeat
  // should keep inserting.
  commit.type = kLynxKeyEventTypeDown;
  commit.physical = 0;
  commit.logical = 0;
  commit.character = text.c_str();
  commit.synthesized = true;
  renderer_->SendKeyEvent(&commit);
}

void LynxWindow::SendPointerEvent(lynx_pointer_phase_e phase,
                                  lynx_pointer_signal_kind_e signal_kind,
                                  double x, double y, double scroll_delta_x,
                                  double scroll_delta_y, bool precise) {
  if (!renderer_ || !window_) {
    return;
  }
  const double scale = window_->scale();
  lynx_pointer_event_t event = {};
  event.struct_size = sizeof(event);
  event.phase = phase;
  event.timestamp = static_cast<size_t>(NowMicroseconds());
  // The embedder pointer protocol uses physical pixels.
  event.x = x * scale;
  event.y = y * scale;
  event.device = 0;
  event.signal_kind = signal_kind;
  event.scroll_delta_x = scroll_delta_x * scale;
  event.scroll_delta_y = scroll_delta_y * scale;
  event.device_kind = kLynxPointerDeviceKindMouse;
  event.buttons = pressed_buttons_;
  event.scale = 1.0;
  event.is_precise_scroll = precise ? 1 : 0;
  renderer_->SendPointerEvent(&event);
}

}  // namespace lynx
