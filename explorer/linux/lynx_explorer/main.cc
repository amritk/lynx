// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "explorer/linux/lynx_explorer/lynx_window.h"
#include "explorer/linux/lynx_explorer/module/explorer_module.h"
#include "explorer/linux/lynx_explorer/module/explorer_module_host.h"
#include "explorer/linux/lynx_explorer/wayland/wayland_app.h"
#include "platform/embedder/http_service/lynx_http_service_impl.h"
#include "platform/embedder/public/lynx_env.h"
#include "platform/embedder/public/lynx_service_center.h"

namespace {

constexpr unsigned int kDefaultWindowWidth = 800;
constexpr unsigned int kDefaultWindowHeight = 600;

// Set once, before anything can start a socket, and read from a signal
// handler afterwards.
lynx::wayland::WaylandApp* g_app = nullptr;

// Asks the loop to stop, then restores the default disposition so a second
// signal kills immediately -- shutdown joins engine threads, which is not
// instant, and a user pressing ctrl-C twice means it. Fetches are not waited
// on: their workers are detached and the process leaves without running
// static destructors, which is what keeps an in-flight fetch from holding
// shutdown open.
extern "C" void OnTerminationSignal(int signal_number) {
  // Restored so a second signal kills immediately: shutdown joins engine
  // threads, which is not instant, and a user pressing ctrl-C twice means it.
  ::signal(signal_number, SIG_DFL);
  // Quit() only clears an atomic flag and writes an eventfd, both of which are
  // safe from a signal handler. These handlers are installed after g_app is
  // set, so there is no window in which it can be null.
  g_app->Quit();
}

// Installed before anything can open a socket. Kept separate from the
// termination handlers below, which need the app to exist first.
void IgnoreBrokenPipe() {
  // cpp-httplib writes to sockets with no MSG_NOSIGNAL (its send flags are 0),
  // and with BoringSSL the flag could not cover the write anyway, so a peer
  // that closes mid-body raises SIGPIPE. Its default action terminates the
  // process -- and these writes happen on detached worker threads, so a single
  // dropped connection would take the whole window down with no teardown and
  // no diagnostic. EPIPE is returned to the caller instead.
  ::signal(SIGPIPE, SIG_IGN);
}

// Installed only once the app exists, so a signal arriving earlier keeps the
// default disposition and terminates, rather than reaching a handler with
// nothing to stop.
void InstallTerminationHandlers() {
  // Without these, ctrl-C or a session logout kills the process outright and
  // the window and engine are never torn down. SIGHUP is in the list because
  // it is what a terminal or session manager actually sends on logout, which
  // is the case these exist for.
  ::signal(SIGINT, OnTerminationSignal);
  ::signal(SIGTERM, OnTerminationSignal);
  ::signal(SIGHUP, OnTerminationSignal);
}

void PrintUsage(const char* program) {
  std::fprintf(stderr,
               "Usage: %s [bundle]\n"
               "  bundle  A .lynx.bundle to load: an http(s) URL, a file://\n"
               "          URL, or a local file path. Defaults to the bundled\n"
               "          homepage card.\n",
               program);
}

}  // namespace

int main(int argc, char** argv) {
  IgnoreBrokenPipe();

  std::string url;
  bool have_url = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 ||
        std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (have_url) {
      std::fprintf(stderr,
                   "lynx_explorer: unexpected extra argument '%s'; this takes "
                   "at most one bundle\n",
                   argv[i]);
      PrintUsage(argv[0]);
      return 1;
    }
    url = argv[i];
    have_url = true;
  }

  // Deliberately leaked. BindLynxUiTaskRunner() installs a pointer to this
  // object into a process-global task runner that has no unregister API, and
  // engine threads can post to it during static destruction, after a stack
  // object here would already be gone.
  auto& app = *new lynx::wayland::WaylandApp();
  g_app = &app;
  InstallTerminationHandlers();
  if (!app.Initialize()) {
    std::fprintf(stderr,
                 "lynx_explorer: failed to connect to a Wayland compositor "
                 "(is WAYLAND_DISPLAY set?)\n");
    return 1;
  }
  if (!app.BindLynxUiTaskRunner()) {
    std::fprintf(stderr,
                 "lynx_explorer: failed to register the Lynx UI task runner\n");
    return 1;
  }

  auto& lynx_env = lynx::pub::LynxEnv::GetInstance();
  lynx_env.SetDevtoolEnabled(false);

  // Back NativeModules.ExplorerModule, which the homepage card calls for
  // navigation, its settings toggles and its preference storage. Registered
  // before any LynxView exists, as on Windows and macOS: the runtime reads
  // the registry when a card first asks for the module.
  lynx_env.RegisterNativeModule("ExplorerModule",
                                lynx::example::ExplorerModuleCreator, nullptr);

  // Back JS fetch() with the shared native HTTP service, as the Windows and
  // macOS explorers do.
  lynx::pub::LynxServiceCenter::GetInstance().RegisterService(
      std::make_shared<lynx::embedder::LynxHttpServiceImpl>());

  // The window is owned by the module host rather than by this frame:
  // ExplorerModule.openSchema() navigates by replacing it, which it can only
  // do if it owns the lifetime. See explorer_module_host.h.
  auto& module_host = lynx::example::ExplorerModuleHost::GetInstance();
  lynx::LynxWindow* window = module_host.CreateWindow(
      &app, kDefaultWindowWidth, kDefaultWindowHeight, "Lynx Explorer");
  if (window == nullptr) {
    std::fprintf(stderr, "lynx_explorer: failed to create the Lynx window\n");
    return 1;
  }

  // With no argument the explorer opens its bundled homepage card.
  //
  // A refused load has to end the process rather than fall through. Nothing
  // is presented until the first frame reaches eglSwapBuffers, so a window
  // that never loaded is never mapped -- and Quit() is reachable only from a
  // close request on a mapped window or from a signal, which would leave the
  // process sitting in epoll with no UI and no way out but ctrl-C. The same
  // reasoning already guards the navigation path in ExplorerModuleHost.
  if (!window->LoadTemplate(url)) {
    module_host.DestroyWindow();
    return 1;
  }

  const bool display_ok = app.Run();

  // Tears the window, and with it the LynxView, down before the process ends.
  module_host.DestroyWindow();

  if (!display_ok) {
    // Distinguished from a clean quit so that a session manager, a wrapper
    // script or a test harness can tell "the user closed the window" from
    // "the compositor went away underneath us".
    std::fprintf(stderr,
                 "lynx_explorer: lost the connection to the Wayland "
                 "compositor\n");
  }

  // Leaves without running static destructors, after flushing everything this
  // process buffered itself.
  //
  // Returning from main() destroys function-local statics while engine threads
  // that nothing joins are still using them. The event-report thread is a
  // NoDestructor<fml::Thread> that is never stopped, and it calls into the
  // process-global service map on every report; destroying that map underneath
  // it faults, reproducibly within a few seconds of a navigation. Detached
  // fetch workers are in the same position, and can be inside BoringSSL when
  // the process ends.
  //
  // Everything this explorer owns has already been torn down above, and the
  // preference file is written with a rename that completes before its save
  // returns, so nothing here needs static destruction to run. This mirrors the
  // reasoning that already leaks the WaylandApp deliberately: state shared with
  // threads that outlive main() must not be destroyed while they can still
  // reach it.
  std::fflush(nullptr);
  _exit(display_ok ? 0 : 1);
}
