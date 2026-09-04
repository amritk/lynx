// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

#include "explorer/linux/lynx_explorer/wayland/wayland_clipboard.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_input.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_window.h"
#include "platform/embedder/public/capi/lynx_windowless_renderer_capi.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace lynx {
namespace wayland {

namespace {

uint64_t NowNanoseconds() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

void OnWmBasePing(void* data, xdg_wm_base* wm_base, uint32_t serial) {
  xdg_wm_base_pong(wm_base, serial);
}

const xdg_wm_base_listener kWmBaseListener = {
    OnWmBasePing,
};

void OnOutputGeometry(void* data, wl_output* output, int32_t x, int32_t y,
                      int32_t physical_width, int32_t physical_height,
                      int32_t subpixel, const char* make, const char* model,
                      int32_t transform) {}

void OnOutputMode(void* data, wl_output* output, uint32_t flags, int32_t width,
                  int32_t height, int32_t refresh) {}

void OnOutputDone(void* data, wl_output* output) {}

void OnOutputScale(void* data, wl_output* output, int32_t factor) {
  static_cast<WaylandApp*>(data)->SetOutputScale(output, factor);
}

const wl_output_listener kOutputListener = {
    OnOutputGeometry,
    OnOutputMode,
    OnOutputDone,
    OnOutputScale,
};

}  // namespace

WaylandApp::WaylandApp() : loop_thread_id_(std::this_thread::get_id()) {}

WaylandApp::~WaylandApp() {
  input_.reset();
  if (egl_display_ != EGL_NO_DISPLAY) {
    eglTerminate(egl_display_);
  }
  for (auto& [name, output] : outputs_) {
    wl_output_destroy(output);
  }
  if (decoration_manager_) {
    zxdg_decoration_manager_v1_destroy(decoration_manager_);
  }
  if (shm_) {
    wl_shm_destroy(shm_);
  }
  if (wm_base_) {
    xdg_wm_base_destroy(wm_base_);
  }
  if (compositor_) {
    wl_compositor_destroy(compositor_);
  }
  if (registry_) {
    wl_registry_destroy(registry_);
  }
  if (display_) {
    wl_display_disconnect(display_);
  }
  if (wakeup_fd_ >= 0) {
    close(wakeup_fd_);
  }
  if (epoll_fd_ >= 0) {
    close(epoll_fd_);
  }
}

// static
void WaylandApp::OnRegistryGlobal(void* data, wl_registry* registry,
                                  uint32_t name, const char* interface,
                                  uint32_t version) {
  auto* app = static_cast<WaylandApp*>(data);
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    app->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(
        registry, name, &wl_compositor_interface, std::min(version, 4u)));
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    app->wm_base_name_ = name;
    app->wm_base_ = static_cast<xdg_wm_base*>(wl_registry_bind(
        registry, name, &xdg_wm_base_interface, std::min(version, 2u)));
    xdg_wm_base_add_listener(app->wm_base_, &kWmBaseListener, app);
  } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
             0) {
    app->decoration_manager_name_ = name;
    app->decoration_manager_ =
        static_cast<zxdg_decoration_manager_v1*>(wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1));
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    app->shm_name_ = name;
    app->shm_ = static_cast<wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    app->seat_name_ = name;
    if (app->input_) {
      app->input_->BindSeat(registry, name, version);
      // The clipboard needs both the seat and the manager, and the registry
      // announces them in whichever order it likes, so both arms try.
      if (app->clipboard_) {
        app->clipboard_->SetSeat(app->input_->seat());
      }
    }
  } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
    app->data_device_manager_name_ = name;
    if (app->clipboard_) {
      app->clipboard_->BindManager(registry, name, version);
      if (app->input_) {
        app->clipboard_->SetSeat(app->input_->seat());
      }
    }
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    auto* output = static_cast<wl_output*>(wl_registry_bind(
        registry, name, &wl_output_interface, std::min(version, 2u)));
    app->outputs_[name] = output;
    app->output_scales_[output] = 1;
    if (wl_output_get_version(output) >= 2) {
      wl_output_add_listener(output, &kOutputListener, app);
    }
  }
}

// static
void WaylandApp::OnRegistryGlobalRemove(void* data, wl_registry* registry,
                                        uint32_t name) {
  auto* app = static_cast<WaylandApp*>(data);
  auto it = app->outputs_.find(name);
  if (it != app->outputs_.end()) {
    wl_output* output = it->second;
    // Drop the pointer from every window first: libwayland recycles proxy
    // allocations, so a stale entry could later alias a different output and
    // silently give the window that display's scale.
    const std::vector<WaylandWindow*> windows = app->windows_;
    for (WaylandWindow* window : windows) {
      // Re-checked rather than trusted: the copy protects the iteration from
      // being invalidated, but not from a callback here destroying a window
      // that appears later in it.
      if (std::find(app->windows_.begin(), app->windows_.end(), window) ==
          app->windows_.end()) {
        continue;
      }
      window->OnOutputRemoved(output);
    }
    app->output_scales_.erase(output);
    wl_output_destroy(output);
    app->outputs_.erase(it);
    return;
  }
  // The other globals this binds are singletons. Leaving their removal
  // unhandled would keep pointers to destroyed resources: the next copy would
  // reach a dead wl_data_device and the compositor would answer an invalid
  // object by killing the process, and a re-advertised seat would never be
  // bound because BindSeat() short-circuits on a non-null seat, leaving input
  // silently dead for the rest of the session.
  if (name == app->seat_name_) {
    app->seat_name_ = kNoGlobalName;
    if (app->clipboard_) {
      // Derived from the seat, so it goes before the seat itself.
      app->clipboard_->UnbindSeat();
    }
    if (app->input_) {
      app->input_->UnbindSeat();
    }
    return;
  }
  if (name == app->data_device_manager_name_) {
    app->data_device_manager_name_ = kNoGlobalName;
    if (app->clipboard_) {
      app->clipboard_->UnbindManager();
    }
    return;
  }
  if (name == app->decoration_manager_name_) {
    app->decoration_manager_name_ = kNoGlobalName;
    // The manager is only needed while a toplevel is being created; existing
    // decorations stay valid, so the pointer is simply forgotten.
    app->decoration_manager_ = nullptr;
    return;
  }
  if (name == app->shm_name_ || name == app->wm_base_name_) {
    // Losing either of these means no new cursor buffers and no new windows.
    // Nothing here can recover from that, and continuing would use a
    // destroyed resource, so stop rather than carry on in a broken state.
    std::fprintf(stderr,
                 "lynx_explorer: the compositor withdrew a global this "
                 "explorer requires; exiting\n");
    app->Quit();
  }
}

bool WaylandApp::Initialize() {
  display_ = wl_display_connect(nullptr);
  if (!display_) {
    return false;
  }

  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  wakeup_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (epoll_fd_ < 0 || wakeup_fd_ < 0) {
    return false;
  }
  struct epoll_event display_event = {};
  display_event.events = EPOLLIN;
  display_event.data.fd = wl_display_get_fd(display_);
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wl_display_get_fd(display_),
            &display_event);
  struct epoll_event wakeup_event = {};
  wakeup_event.events = EPOLLIN;
  wakeup_event.data.fd = wakeup_fd_;
  epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &wakeup_event);

  input_ = std::make_unique<WaylandInput>(this);
  clipboard_ = std::make_unique<WaylandClipboard>(this);

  registry_ = wl_display_get_registry(display_);
  static const wl_registry_listener kRegistryListener = {
      OnRegistryGlobal,
      OnRegistryGlobalRemove,
  };
  wl_registry_add_listener(registry_, &kRegistryListener, this);
  // First roundtrip discovers globals, second delivers their initial events
  // (seat capabilities, output scales).
  wl_display_roundtrip(display_);
  wl_display_roundtrip(display_);

  if (!compositor_ || !wm_base_) {
    return false;
  }
  return InitializeEGL();
}

bool WaylandApp::InitializeEGL() {
  egl_display_ =
      eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
  if (egl_display_ == EGL_NO_DISPLAY) {
    return false;
  }
  if (!eglInitialize(egl_display_, nullptr, nullptr)) {
    return false;
  }
  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    return false;
  }
  const EGLint config_attributes[] = {
      // clang-format off
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_STENCIL_SIZE, 8,
      EGL_NONE,
      // clang-format on
  };
  EGLint config_count = 0;
  if (!eglChooseConfig(egl_display_, config_attributes, &egl_config_, 1,
                       &config_count) ||
      config_count == 0) {
    return false;
  }
  return true;
}

bool WaylandApp::BindLynxUiTaskRunner() {
  lynx_windowless_ui_task_runner_config_t config = {};
  config.struct_size = sizeof(config);
  config.user_data = this;
  config.runs_on_current_thread_callback = [](void* user_data) -> bool {
    return static_cast<WaylandApp*>(user_data)->RunsOnUiThread();
  };
  config.post_task_callback = [](lynx_task_t task, uint64_t target_time_nanos,
                                 void* user_data) {
    auto* app = static_cast<WaylandApp*>(user_data);
    app->PostTaskAt([task]() { lynx_windowless_run_ui_task(task); },
                    target_time_nanos);
  };
  return lynx_windowless_set_global_ui_task_runner(&config);
}

bool WaylandApp::RunsOnUiThread() const {
  return std::this_thread::get_id() == loop_thread_id_;
}

void WaylandApp::PostTask(std::function<void()> task) {
  PostTaskAt(std::move(task), NowNanoseconds());
}

void WaylandApp::PostDelayedTask(std::function<void()> task,
                                 uint64_t delay_ns) {
  PostTaskAt(std::move(task), NowNanoseconds() + delay_ns);
}

void WaylandApp::PostTaskAt(std::function<void()> task,
                            uint64_t target_time_ns) {
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    task_queue_.push({target_time_ns, task_sequence_++, std::move(task)});
  }
  Wakeup();
}

void WaylandApp::Wakeup() {
  const uint64_t value = 1;
  [[maybe_unused]] ssize_t written = write(wakeup_fd_, &value, sizeof(value));
}

int WaylandApp::RunExpiredTasks() {
  // Tasks posted while draining are left for the next pass. Without this, a
  // task that posts another due task would keep the loop here indefinitely and
  // the Wayland connection would never be read, flushed, or ponged.
  uint64_t batch_end;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    batch_end = task_sequence_;
  }
  while (true) {
    std::function<void()> task;
    {
      std::lock_guard<std::mutex> lock(task_mutex_);
      if (task_queue_.empty()) {
        return -1;
      }
      const ScheduledTask& next = task_queue_.top();
      if (next.sequence >= batch_end) {
        // Service Wayland, then come straight back for these.
        return 0;
      }
      const uint64_t now = NowNanoseconds();
      if (next.target_time_ns > now) {
        const uint64_t delta_ns = next.target_time_ns - now;
        // Round up so we never spin on a not-yet-due task.
        return static_cast<int>(
            std::min<uint64_t>(delta_ns / 1000000ull + 1, 1000));
      }
      task = std::move(const_cast<ScheduledTask&>(task_queue_.top()).task);
      task_queue_.pop();
    }
    task();
  }
}

// Flushes buffered requests, waiting for writability when the socket is full.
// Returns false if the connection is unusable.
bool WaylandApp::FlushDisplay() {
  const int display_fd = wl_display_get_fd(display_);
  while (wl_display_flush(display_) == -1) {
    if (errno == EPIPE) {
      // Not fatal here, deliberately. When the compositor rejects a request it
      // writes the error and closes the socket, so the flush that follows
      // fails with EPIPE; returning now would break out of the loop before
      // anything reads that error, and the user would be told only that the
      // connection was lost. Falling through lets the read pick up the real
      // wl_display.error, which is the difference between "lost the
      // compositor" and naming the protocol violation. libwayland's own
      // wl_display_dispatch_queue does the same for the same reason.
      return true;
    }
    if (errno != EAGAIN) {
      return false;
    }
    // Wait on the display fd alone with poll(), never on the app's epoll set:
    // that set also holds the level-triggered wakeup eventfd, and nothing
    // drains it until the main loop further down. Waiting on it here returns
    // immediately for as long as any task has been posted, which pins a core
    // until the compositor drains the socket.
    struct pollfd writable = {};
    writable.fd = display_fd;
    writable.events = POLLOUT;
    const int ready = poll(&writable, 1, 1000);
    const int poll_errno = errno;
    if (ready < 0 && poll_errno != EINTR) {
      return false;
    }
    if (ready > 0 && (writable.revents & (POLLERR | POLLHUP)) != 0) {
      // Same reasoning as EPIPE above: the socket is gone, but the error the
      // compositor sent before closing it has not been read yet. Upstream
      // deliberately has no such check so that the read still runs.
      return true;
    }
  }
  return true;
}

bool WaylandApp::Run() {
  bool display_ok = true;
  // running_ is not reset here: a Quit() delivered before Run() starts (the
  // compositor can send xdg_toplevel.close while the window is still being
  // constructed) must not be swallowed.
  const int display_fd = wl_display_get_fd(display_);
  while (running_) {
    int timeout_ms = RunExpiredTasks();
    if (!running_) {
      break;
    }

    while (wl_display_prepare_read(display_) != 0) {
      wl_display_dispatch_pending(display_);
    }
    if (!FlushDisplay()) {
      wl_display_cancel_read(display_);
      display_ok = false;
      running_ = false;
      break;
    }

    struct epoll_event events[8];
    const int event_count = epoll_wait(
        epoll_fd_, events, sizeof(events) / sizeof(events[0]), timeout_ms);
    bool display_readable = false;
    for (int i = 0; i < event_count; ++i) {
      if (events[i].data.fd == display_fd) {
        display_readable = true;
      } else if (events[i].data.fd == wakeup_fd_) {
        uint64_t value = 0;
        [[maybe_unused]] ssize_t bytes =
            read(wakeup_fd_, &value, sizeof(value));
      }
    }
    if (display_readable) {
      wl_display_read_events(display_);
      wl_display_dispatch_pending(display_);
    } else {
      wl_display_cancel_read(display_);
    }
    if (wl_display_get_error(display_) != 0) {
      display_ok = false;
      running_ = false;
    }
  }
  return display_ok;
}

void WaylandApp::Quit() {
  running_ = false;
  // Break epoll_wait(), which otherwise blocks indefinitely on an empty task
  // queue.
  Wakeup();
}

int32_t WaylandApp::GetOutputScale(wl_output* output) const {
  auto it = output_scales_.find(output);
  return it != output_scales_.end() ? it->second : 1;
}

void WaylandApp::SetOutputScale(wl_output* output, int32_t scale) {
  if (output_scales_[output] == scale) {
    return;
  }
  output_scales_[output] = scale;
  // A display whose scale is reconfigured while the window is already on it
  // never sends enter/leave, so the windows have to be told directly. Iterate
  // a copy: this reaches engine code that may destroy a window, which would
  // erase from windows_ mid-iteration.
  const std::vector<WaylandWindow*> windows = windows_;
  for (WaylandWindow* window : windows) {
    window->OnOutputScaleChanged(output);
  }
}

void WaylandApp::RegisterWindow(WaylandWindow* window) {
  windows_.push_back(window);
}

void WaylandApp::UnregisterWindow(WaylandWindow* window) {
  windows_.erase(std::remove(windows_.begin(), windows_.end(), window),
                 windows_.end());
  if (input_) {
    input_->OnWindowDestroyed(window);
  }
}

WaylandWindow* WaylandApp::FindWindow(wl_surface* surface) const {
  for (WaylandWindow* window : windows_) {
    if (window->surface() == surface) {
      return window;
    }
  }
  return nullptr;
}

}  // namespace wayland
}  // namespace lynx
