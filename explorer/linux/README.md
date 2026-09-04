# Lynx Explorer for Linux (Wayland)

A desktop host for Lynx on Linux. It opens an xdg-shell (Wayland) window with
an EGL surface and renders a `LynxView` through the embedder's windowless GL
renderer, wiring mouse, scroll wheel, and keyboard input into Lynx.

## Status

What works:

- Wayland windowing (xdg-shell toplevel, close, interactive resize)
- Server-side window decorations via `xdg-decoration-unstable-v1`, so the
  compositor draws the title bar and close button
- HiDPI output scaling (`wl_output` scale, `wl_surface.set_buffer_scale`)
- GPU rendering through EGL / OpenGL ES with the `GLDirect` windowless
  renderer, drawing straight into the window's default framebuffer
- Mouse pointer, buttons, and scroll (with touchpad precise-scroll detection)
- Keyboard input through `xkbcommon` (layout-aware text, common physical and
  logical key mapping) with compositor-configured key repeat
- Typing into text fields. The engine's editable only inserts text from an
  event flagged synthesized, expecting a platform IME to supply it, and the
  windowless renderer has no IME hook, so the host plays that part itself:
  a printable key sends its real event and then a synthesized text commit.
  This mirrors `oliver/node-lynx`'s windowed macOS host, which sends a second
  event from `insertText:`. It is *not* what `platform/darwin/macos` does --
  that goes through `FlutterTextInputPlugin` and produces no second key
  event -- so a card with its own `keydown` handler sees two events per
  printable key here and one there.
- Loading bundles from http(s) URLs, `file://` URLs, or local paths, and a
  bundled homepage card when started with no argument. A bundle that cannot
  be fetched or read is reported on stderr; note that the window is then never
  mapped, so the process runs on with nothing on screen rather than exiting
- A scaled mouse cursor, honouring `XCURSOR_THEME` and `XCURSOR_SIZE` and
  reloading when the pointer moves to a differently scaled output
- The system clipboard, over `wl_data_device`, in both directions. Copying
  claims the selection and serves it to other clients; the current selection
  is pulled in and cached whenever it changes, so the engine's synchronous
  clipboard getter never waits on whichever application owns it. When the
  compositor offers no `wl_data_device_manager` this falls back to a
  clipboard private to this process. Note that a selection disappears when
  the application offering it exits, which is how Wayland works and unlike
  X11 with a clipboard manager running
- `NativeModules.ExplorerModule`, which the homepage card calls for
  navigation, its settings toggles and its preference storage. Navigating
  keeps the window and replaces only the `LynxView` inside it, so the toplevel
  stays mapped across a page change. The view still has to be replaced,
  because a second `LoadTemplate` on a used `LynxView` re-evaluates the new
  bundle's main-thread script in the previous context and fails
- The showcase cards, built and staged next to the executable, so the
  homepage's "Lynx Showcases" entry works from a plain build
- Theme persistence. `saveThemePreferences` writes the choice to disk and
  `LynxWindow::LoadTemplate` injects it back as `preferredTheme` in the card's
  global props, which is the only channel the card reads a theme from. Note
  that the props are supplied per load, so a change applies from the next
  navigation or restart rather than repainting the page in place
- Local resource reads confined to the directories the host chose: the staged
  `resources/` tree, plus the directory of a bundle named on the command line
  or opened through `openSchema`. A bundle is untrusted code, and every
  resource type -- bundles, images, fonts, lazy components -- resolves through
  one fetcher, so without a root a card could read (and, via
  `lynx.QueryComponent`, run) any file the process can open, with the bytes
  coming back to JS as a string. `local://` also rejects `..`, which the
  leading-slash strip alone did not

### What has actually been verified

Three independent compositors were used, which matters because they are
separate protocol implementations, not three builds of one. Each covers what
the others cannot. weston and sway were driven headless in CI-like containers;
Hyprland was driven both on real hardware and headless, and is what closes the
"never run on a real GPU or in a real desktop session" gap the earlier rounds
carried:

- **weston** (libweston), headless with the GL renderer, covers rendering.
  Frames are captured with `weston-screenshooter` and compared against a
  per-run baseline of the bare compositor, using region colour-count and
  standard-deviation checks so that a flat fill cannot pass as rendered UI. A
  current run reports 892 distinct colours over the 800x600 region the client
  changed. Weston advertises no `wl_seat`, so it cannot exercise input, and it
  sends `xdg_toplevel.configure(0, 0)` — telling the client to choose its own
  size, a path sway never takes. It also raises zero protocol errors across a
  run and exits cleanly on `SIGINT`, which is worth stating because libwayland
  calls `wl_abort()` on the client for any protocol violation.
- **sway** (wlroots), headless, covers input and decorations. A helper client
  feeds events through the wlroots virtual-pointer and virtual-keyboard
  protocols, which is what gives the session a `wl_seat`, so this is where the
  pointer, button, scroll, keyboard and key-repeat paths have been driven end
  to end. sway is also the only compositor here that advertises
  `zxdg_decoration_manager_v1`, so it is where the decoration handshake runs:
  the client binds the manager, calls `get_toplevel_decoration`, requests
  `set_mode(server_side)` and receives `configure(server_side)` back. It runs
  the pixman renderer under `LIBGL_ALWAYS_SOFTWARE`, so it contributes no GL
  or pixel evidence.

- **Hyprland** 0.56.2 (aquamarine), on a real desktop session and on a real GL
  driver -- Mesa `radeonsi` on amdgpu -- and separately on a nested instance of
  the same build driven through a headless output. This is the only rig here
  with a non-llvmpipe driver, and the only one that is a real desktop session
  rather than a container. It is the second implementation to run the
  decoration handshake, and it grants `server_side`. Unlike weston it sends
  real non-zero `xdg_toplevel.configure` sizes, and re-sends them when the
  output changes, so the reconfigure path was exercised across 2541x1378,
  1916x1076 and 1596x896 with correct reflow at each size. Rendering evidence
  is stronger than the flat-UI checks above: a showcase card of photographic
  images reports 167495 distinct colours, and scrolling it changes 28% of the
  frame. Pointer motion, click-to-focus, wheel scroll, typing, and the
  `Ctrl+A`/`Ctrl+C`/`Ctrl+V` hot keys all work, as does a clipboard round trip
  in both directions against `wl-clipboard`. At `wl_output` scale 2 the client
  sends `set_buffer_scale(2)` and renders crisp; at fractional 1.5 the
  compositor rounds up and the client uses scale 2, which is the documented
  behaviour below. Zero protocol errors across every run, and a clean exit
  status 0 both from the window close request and from `SIGTERM`. The
  file-descriptor leak recorded under Known issues reproduces here at exactly
  +2 per navigation.

Note that `weston-screenshooter` only works under the GL renderer; started with
weston's default pixman renderer it asserts on a zero-width buffer, so the
rendering rig must pass `--renderer=gl`.

A warning for anyone building an input rig: **do not drive this with `wtype`.**
It synthesises its own keymap and assigns keysyms to low evdev keycodes, so the
keycode carrying a typed letter is also `KEY_ESC`, and the host correctly reads
the release as Escape and ends editing -- which looks exactly like "only the
first character is ever inserted". It also leaves `KEY_LEFTMETA` depressed in
the seat when its virtual keyboard goes away, which latches Super and makes
every later keystroke look like a shortcut, poisoning the rest of the session.
A `zwp_virtual_keyboard_v1` client that uploads the ordinary `evdev`/`us` keymap
and presses real evdev keycodes reproduces none of this.

Still *not* covered:

- **the other desktop compositors** — GNOME/Mutter and KDE/KWin have never seen
  this code. Hyprland covers wlroots-lineage tiling and a real GPU, but not
  Mutter's or KWin's protocol implementations, and neither of those grants
  server-side decorations the way Hyprland and sway do;
- **decorations beyond the handshake** — the protocol exchange is confirmed
  under sway and Hyprland, but neither has been checked for what it actually
  draws, and the client-side-decoration fallback (for compositors that refuse
  server-side mode) still does not exist;
- **the JavaScript engine and ICU** — the bundled card renders identically with
  `lynx_core.js` or `icudtl.dat` removed, so a rendering check cannot detect
  either being broken.

`GLDirect` renders directly on the host GL context, which is fast but, per
`clay/public/clay.h`, cannot support components backed by external textures
such as video and canvas. Switching the renderer type to `kRendererTypeGL` in
`wayland_lynx_renderer.cc` trades speed for a shared-image-sink readback that
supports them.

Frame pacing currently comes from Lynx's own vsync fallback, a request-driven
60 Hz timer (`platform/embedder/vsync_monitor_fallback.cc`), with
`eglSwapInterval(0)` so the raster thread never blocks in `eglSwapBuffers`.
This is correct but display-agnostic.

Not yet implemented (contributions welcome):

- X11/XWayland fallback for sessions without a Wayland compositor
- Vsync driven by `wl_surface.frame` callbacks, so frames follow the actual
  output refresh rate instead of a fixed 60 Hz timer
- `wp_fractional_scale_v1`. At a fractional scale the compositor rounds up --
  1.5 is reported as `wl_output.scale` 2 -- so the layout is right but the
  window is rendered larger than the display needs and scaled back down
- A cursor at exactly the requested size on every theme. Xcursor returns the
  nearest size a theme actually ships rather than resizing, so on an output
  whose scale does not divide that size the cursor is drawn larger than asked
  for; see `SetCursor` in `wayland/wayland_input.cc`
- IME / text-input protocol (`zwp_text_input_v3`), so no compose key, no
  dead keys and no CJK input method; only directly typed characters work
- Client-side decorations, for compositors that do not implement
  `xdg-decoration` or that insist on client-side mode; on those the window
  currently has no frame of its own
- Pausing the view when the window is not activated: `xdg_toplevel.configure`
  states are ignored, so `OnEnterBackground` is never called and cards keep
  running while minimised or occluded
- DevTool / LogBox integration, and the switch pages the settings screen
  links to: the module resolves their URLs correctly but this build stages no
  devtool bundles for them to open
- Any bound on concurrent fetches: each resource request detaches a thread,
  matching the Windows explorer

## Known issues

Three defects are known and unfixed, each because the safe fix is not small.

**Every in-app navigation leaks two file descriptors, and the leak is in the
engine rather than in this port.** `openSchema` destroys the `LynxView` and
builds a new one, and each rebuild retains the raster thread's
`fml::MessageLoopImpl` after that thread has exited, keeping its `epoll` and
`timerfd` descriptors open. Measured at exactly +2 per navigation with no
deviation -- 16 descriptors at launch, 516 after 250 navigations -- along with
about 58 kB of resident memory per navigation from the same retained object.
The `thread_local` holding the loop is destroyed on thread exit, so something
is keeping a `RefPtr` to the loop alive; `clay/lynx_adaptor` captures task
runners by `RefPtr` in at least one place that never releases. Fixing it means
changing shared engine lifetime code that every platform uses, which is beyond
what a port branch should do on its own analysis.

What it costs in practice: on a desktop where `RLIMIT_NOFILE` is the usual
1024, roughly 500 navigations exhaust the process. The window itself is no
longer re-created per navigation, so it no longer vanishes at that point --
what fails instead is whatever the exhausted process next asks a descriptor
for, starting with the engine threads a new view needs, and a navigation that
cannot build its view leaves the previous page on screen and reports the
failure on stderr. `SIGTERM` still exits cleanly.

**Enter does not insert a newline in a multi-line field.** xkb reports Enter
as `\r`, which is a control byte, so the host sends no text commit for it.
clay's editable has exactly one path that turns Enter into a newline
(`EditableView::OnKeyEventInternal`), and it requires a *synthesized* event
whose logical key is Enter and whose character is `"\r"`, plus
`max_lines_ > 1`. Sending that unconditionally would be worse than the
current behaviour: a single-line field falls through the same case and would
take a literal carriage return into its value. The host cannot see
`max_lines_`, so fixing this needs either that state exposed or a separate
commit path for line breaks. Single-line fields are unaffected -- they get
their confirm action from a different path.

**Keys pressed immediately after clicking into a field can be dropped.** The
host decides whether to send a text commit by reading a flag the engine sets
from `ShowTextInput`, and it reads it when the Wayland event arrives. The
click that focuses the field is dispatched to the engine asynchronously, so
with enough UI-thread backlog -- a heavy card's first layout, say -- a
keystroke can be translated while the flag is still false. The real key event
still goes out, but it inserts nothing, so the character is lost rather than
reordered. The reverse case is harmless: a stale-true flag is rejected by the
editable because it is not editing.


`third_party/httplib`'s Linux DNS path (`getaddrinfo_a` in `httplib.cc`) has a
use-after-return that predates this port. When `gai_suspend()` times out, the
code calls `gai_cancel()` and returns immediately. `gai_cancel()` returns
`EAI_NOTCANCELED` for a lookup already in flight, and glibc's resolver thread
then writes its result into the `struct gaicb` — a local of the frame that has
already returned. So a DNS lookup slower than the configured timeout can
corrupt the caller's stack from another thread.

It is not patched here because a correct fix means heap-allocating the
`gaicb` together with copies of the node, service and hints it points at, and
deliberately leaking that block when cancellation fails — surgery on a shared
dependency that this port cannot exercise. Anything reaching a name server
slow enough to hit the timeout is exposed, on every platform that compiles
this branch, not only Linux desktop.

While following redirects, httplib also discards the 3xx response's own body
without offering it to the caller's content receiver, so a response size cap
expressed through the receiver does not bound it, and the read timeout is
per-`recv` rather than per-transfer. `lynx_http_service_impl_linux.cc` no
longer relies on httplib for this at all -- it follows redirects itself, so it
can refuse an https-to-http downgrade and drop credential headers on a
cross-origin hop, neither of which httplib does -- and `httplib_client.cc`
sets an absolute `set_max_timeout()` ceiling on the whole transfer. The body
of a discarded 3xx is still dropped rather than buffered, so the residual
effect is a stalled fetch bounded by that ceiling, not memory growth.

## System requirements

### To build

- Node.js and pnpm, from `buildtools` after `source tools/envsetup.sh`, plus a
  `pnpm install` at the repository root: the homepage card is built from
  source and its `node_modules` are not committed. This step needs network
  access.

Nothing else needs to be installed. The Wayland, xkbcommon and EGL libraries
and headers come from the Debian sysroot the build already uses
(`build/config/sysroot.gni`), and the Wayland protocol sources are committed
under `lynx_explorer/wayland/protocols/`, so neither `wayland-scanner` nor the
`wayland-protocols` definitions are required. See that directory's README
before regenerating them: the sysroot ships libwayland 1.17, so the protocol
sources must be generated with a scanner older than 1.20.

### To run

The sysroot supplies the build only; at runtime the executable loads the
host's libraries. It needs a Wayland session (`$WAYLAND_DISPLAY` set) and:

- `libEGL` with a working driver (Mesa is fine, including its llvmpipe
  software fallback)
- `libwayland-client`, `libwayland-cursor`, `libwayland-egl`, `libxkbcommon`
- `libepoxy` and `libexpat`, which `liblynx.so` requires

On Debian/Ubuntu: `libegl1 libwayland-client0 libwayland-cursor0
libwayland-egl1 libxkbcommon0 libepoxy0 libexpat1`.

## Build

From the repository root:

```sh
source tools/envsetup.sh
tools/hab sync . --target clay

buildtools/gn/gn gen out/linux --args='target_os="linux" target_cpu="x64"
  desktop_enable_embedder_layer=true enable_clay=true enable_clay_standalone=true
  is_headless=true is_debug=false jsengine_type="quickjs" use_flutter_cxx=true
  enable_napi_binding=true enable_inspector=false enable_lepusng_worklet=true
  enable_svg=true allow_deprecated_api_calls=true disable_visibility_hidden=true
  use_ndk_static_cxx=false enable_linker_map=false skia_enable_flutter_defines=true
  skia_use_dng_sdk=false skia_use_sfntly=false skia_enable_pdf=false
  skia_enable_svg=true skia_enable_skottie=true skia_use_x11=false
  skia_use_wuffs=true skia_use_expat=true skia_use_fontconfig=false
  skia_use_icu=true skia_gl_standard="" clay_enable_skshaper=true'

buildtools/ninja/ninja -C out/linux explorer/linux/lynx_explorer:lynx_explorer
```

(Write the `--args` value on a single line; it is wrapped here for
readability. `platform/linux/build_release.py` uses a similar set for the
headless SDK, but not an identical one: it does not set
`desktop_enable_embedder_layer`, and it adds release-only flags such as
`is_official_build` and `stripped_symbols`.)

`enable_inspector=false` costs one thing worth knowing about: it also disables
LogBox, the on-screen error overlay, because `platform/embedder/lynx_view.cc`
gates that on the same flag. A card with a JavaScript error therefore renders
nothing at all here, where the Windows and macOS explorers show a red overlay
naming the error. The only diagnostic is the line
`LynxViewClient::OnReceivedError` prints to stderr, which nobody launching
from a desktop menu will see. The flag stays off regardless, for the reason
below.

`enable_inspector=false` is deliberate and worth keeping. With the inspector
compiled in, the engine's DebugRouter binds a TCP listener on `0.0.0.0:8901`
from the moment the first `LynxView` is created. It has no authentication --
a fixed three-byte header and then Chrome DevTools Protocol messages, which
evaluate JavaScript in the page and can navigate the view -- and because it
binds `INADDR_ANY` rather than loopback it is reachable from any host on the
network, not merely from the local machine. The explorer's own
`SetDevtoolEnabled(false)` does not prevent it: that gates whether the engine
attaches its inspector, not whether the socket is opened. Since this explorer
implements no DevTool integration, the flag is simply off. Verified: with it
off the process opens no listening socket at all.

`is_headless=true` is what the Linux clay backend expects: it selects the
windowless rendering path, where the host — this explorer — owns the native
window and GL context rather than clay creating one. `skia_use_fontconfig` is
off because fontconfig is not vendored in this tree; Skia falls back to its
custom font manager.

## Run

```sh
out/linux/lynx_explorer/lynx_explorer path/to/main.lynx.bundle
# or
out/linux/lynx_explorer/lynx_explorer https://example.com/main.lynx.bundle
```

Running with no argument opens the bundled homepage card.
