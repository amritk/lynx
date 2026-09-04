// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_APP_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_APP_H_

#include <EGL/egl.h>
#include <wayland-client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

struct xdg_wm_base;
struct zxdg_decoration_manager_v1;

namespace lynx {
namespace wayland {

class WaylandInput;
class WaylandClipboard;
class WaylandWindow;

// Owns the Wayland display connection, the globals advertised by the
// compositor, the EGL display, and the application event loop. The event loop
// doubles as the Lynx UI task runner: tasks posted from any thread (via
// PostTask/PostDelayedTask) are executed on the loop thread, ordered by their
// target time.
class WaylandApp {
 public:
  WaylandApp();
  ~WaylandApp();

  WaylandApp(const WaylandApp&) = delete;
  WaylandApp& operator=(const WaylandApp&) = delete;

  // Connects to the Wayland display and initializes EGL. Returns false when
  // no Wayland compositor is reachable or a required global is missing.
  bool Initialize();

  // Registers this app's event loop as the process-global Lynx UI task
  // runner. Must be called on the loop thread, before any LynxView exists.
  bool BindLynxUiTaskRunner();

  // Runs the event loop until Quit() is called.
  // Runs until Quit() or until the compositor connection fails. Returns false
  // in the second case: a caller cannot otherwise tell a user closing the
  // window from the session going away, and nothing else reports it.
  bool Run();
  void Quit();

  // Task posting. Safe to call from any thread.
  void PostTask(std::function<void()> task);
  void PostDelayedTask(std::function<void()> task, uint64_t delay_ns);
  // target_time_ns is an absolute CLOCK_MONOTONIC timestamp in nanoseconds.
  void PostTaskAt(std::function<void()> task, uint64_t target_time_ns);

  bool RunsOnUiThread() const;

  wl_display* display() const { return display_; }
  wl_compositor* compositor() const { return compositor_; }
  xdg_wm_base* wm_base() const { return wm_base_; }
  wl_shm* shm() const { return shm_; }
  zxdg_decoration_manager_v1* decoration_manager() const {
    return decoration_manager_;
  }
  EGLDisplay egl_display() const { return egl_display_; }
  EGLConfig egl_config() const { return egl_config_; }
  WaylandInput* input() const { return input_.get(); }
  WaylandClipboard* clipboard() const { return clipboard_.get(); }

  int32_t GetOutputScale(wl_output* output) const;
  void SetOutputScale(wl_output* output, int32_t scale);

  void RegisterWindow(WaylandWindow* window);
  void UnregisterWindow(WaylandWindow* window);
  // Returns the registered window owning |surface|, or nullptr.
  WaylandWindow* FindWindow(wl_surface* surface) const;
  bool HasWindows() const { return !windows_.empty(); }

 private:
  struct ScheduledTask {
    uint64_t target_time_ns;
    uint64_t sequence;
    std::function<void()> task;
    bool operator>(const ScheduledTask& other) const {
      if (target_time_ns != other.target_time_ns) {
        return target_time_ns > other.target_time_ns;
      }
      return sequence > other.sequence;
    }
  };

  static void OnRegistryGlobal(void* data, wl_registry* registry, uint32_t name,
                               const char* interface, uint32_t version);
  static void OnRegistryGlobalRemove(void* data, wl_registry* registry,
                                     uint32_t name);

  bool InitializeEGL();
  bool FlushDisplay();
  void Wakeup();
  // Runs tasks whose target time has passed; returns the epoll timeout in
  // milliseconds until the next scheduled task (-1 when the queue is empty).
  int RunExpiredTasks();

  wl_display* display_ = nullptr;
  wl_registry* registry_ = nullptr;
  wl_compositor* compositor_ = nullptr;
  xdg_wm_base* wm_base_ = nullptr;
  wl_shm* shm_ = nullptr;
  zxdg_decoration_manager_v1* decoration_manager_ = nullptr;
  std::unordered_map<uint32_t, wl_output*> outputs_;
  // Registry names of the singleton globals, so that a global_remove can be
  // matched against them. wl_registry.global_remove carries only the name, so
  // without these there is nothing to compare and a removal is invisible.
  static constexpr uint32_t kNoGlobalName = 0xffffffffu;
  uint32_t seat_name_ = kNoGlobalName;
  uint32_t data_device_manager_name_ = kNoGlobalName;
  uint32_t shm_name_ = kNoGlobalName;
  uint32_t wm_base_name_ = kNoGlobalName;
  uint32_t decoration_manager_name_ = kNoGlobalName;
  std::unordered_map<wl_output*, int32_t> output_scales_;

  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLConfig egl_config_ = nullptr;

  std::unique_ptr<WaylandInput> input_;
  std::unique_ptr<WaylandClipboard> clipboard_;
  std::vector<WaylandWindow*> windows_;

  std::thread::id loop_thread_id_;
  int epoll_fd_ = -1;
  int wakeup_fd_ = -1;
  // Set before Run() so a Quit() arriving during window construction is not
  // lost; atomic because Quit() is part of the thread-safe surface.
  std::atomic<bool> running_{true};

  mutable std::mutex task_mutex_;
  std::priority_queue<ScheduledTask, std::vector<ScheduledTask>,
                      std::greater<ScheduledTask>>
      task_queue_;
  uint64_t task_sequence_ = 0;
};

}  // namespace wayland
}  // namespace lynx

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_WAYLAND_WAYLAND_APP_H_
