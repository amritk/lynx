// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#include "explorer/linux/lynx_explorer/module/explorer_module.h"

#include <cstdio>
#include <string>
#include <vector>

#include "explorer/linux/lynx_explorer/module/explorer_module_host.h"

#ifdef USE_WEAK_SUFFIX_NAPI
#include "third_party/weak-node-api/headers/weak_napi_defines.h"
#endif

namespace lynx {
namespace example {

namespace {

// A NAPI callback that returns nullptr yields undefined in JS, which is what
// every void method here wants.
constexpr napi_value kUndefined = nullptr;

// The longest string this accepts from a card. Nothing here handles anything
// approaching it -- a URL, a preference key, a theme name -- and without a
// bound the allocation below is sized by the caller, so a hostile or broken
// card could ask for an arbitrarily large one and throw std::bad_alloc out
// through the engine's C frames.
constexpr size_t kMaxStringArgumentBytes = 1u << 20;  // 1 MiB

// Reads argument |index| as a string. Returns false, leaving |value| alone,
// when the argument is missing, is not a string, or is implausibly long --
// the card is typed, so that only happens if a card passes something
// unexpected, and dropping the call is friendlier than throwing.
bool GetStringArgument(napi_env env, size_t argc, const napi_value* argv,
                       size_t index, std::string* value) {
  if (index >= argc) {
    return false;
  }
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, argv[index], &type) != napi_ok || type != napi_string) {
    return false;
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, argv[index], nullptr, 0, &length) !=
      napi_ok) {
    return false;
  }
  if (length > kMaxStringArgumentBytes) {
    return false;
  }
  // napi_get_value_string_utf8 writes a terminator, so the buffer needs one
  // byte more than the string it reports.
  std::vector<char> buffer(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[index], buffer.data(), buffer.size(),
                                 &copied) != napi_ok) {
    return false;
  }
  value->assign(buffer.data(), copied);
  return true;
}

// Unlike GetStringArgument, these two do not type-check: the underlying NAPI
// calls coerce, so a string or undefined becomes a number or false rather
// than being rejected, and a value whose conversion throws propagates that
// exception into JS rather than dropping the call. Both are only reachable
// from methods no shipping card calls; a caller that needs the documented
// drop-on-wrong-type behaviour has to check napi_typeof first.
bool GetInt32Argument(napi_env env, size_t argc, const napi_value* argv,
                      size_t index, int32_t* value) {
  if (index >= argc) {
    return false;
  }
  return napi_get_value_int32(env, argv[index], value) == napi_ok;
}

bool GetBoolArgument(napi_env env, size_t argc, const napi_value* argv,
                     size_t index, bool* value) {
  if (index >= argc) {
    return false;
  }
  return napi_get_value_bool(env, argv[index], value) == napi_ok;
}

napi_value OpenSchema(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string url;
  if (!GetStringArgument(env, argc, argv, 0, &url)) {
    return kUndefined;
  }
  ExplorerModuleHost::GetInstance().OpenSchema(url);
  return kUndefined;
}

// No-op: opening a QR scanner needs a camera and a capture stack this explorer
// does not have. The card calls this from the scan button, so it has to exist
// and it has to not throw; a log makes the dead button visible to whoever
// pressed it.
napi_value OpenScan(napi_env env, napi_callback_info info) {
  std::fprintf(stderr,
               "lynx_explorer: ExplorerModule.openScan() is not implemented on "
               "Linux (no camera capture); type a bundle URL instead\n");
  return kUndefined;
}

// Recorded, not applied. The thread strategy is fixed when the LynxView is
// built, and this explorer builds exactly one at startup, so changing it would
// mean tearing that view down and losing the page. Recording the value keeps
// getSettingInfo() consistent with what the Settings page shows.
napi_value SetThreadMode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t thread_mode = 0;
  if (!GetInt32Argument(env, argc, argv, 0, &thread_mode)) {
    return kUndefined;
  }
  ExplorerModuleHost::GetInstance().SetThreadMode(thread_mode);
  return kUndefined;
}

// Recorded, not applied, for the same reason as setThreadMode(): the preset
// width and height are part of the screen metrics the window owns.
napi_value SwitchPreSize(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool enabled = false;
  if (!GetBoolArgument(env, argc, argv, 0, &enabled)) {
    return kUndefined;
  }
  ExplorerModuleHost::GetInstance().SetPresetSizeEnabled(enabled);
  return kUndefined;
}

// Recorded, not applied. Render nodes are an Android-only rendering path.
napi_value SwitchRenderNode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool enabled = false;
  if (!GetBoolArgument(env, argc, argv, 0, &enabled)) {
    return kUndefined;
  }
  ExplorerModuleHost::GetInstance().SetRenderNodeEnabled(enabled);
  return kUndefined;
}

// Mirrors the iOS shape: { threadMode, preSize, enableRenderNode }.
napi_value GetSettingInfo(napi_env env, napi_callback_info info) {
  napi_value settings = nullptr;
  if (napi_create_object(env, &settings) != napi_ok) {
    return kUndefined;
  }
  ExplorerModuleHost& host = ExplorerModuleHost::GetInstance();

  napi_value thread_mode = nullptr;
  napi_create_int32(env, host.GetThreadMode(), &thread_mode);
  napi_set_named_property(env, settings, "threadMode", thread_mode);

  napi_value preset_size = nullptr;
  napi_get_boolean(env, host.IsPresetSizeEnabled(), &preset_size);
  napi_set_named_property(env, settings, "preSize", preset_size);

  napi_value render_node = nullptr;
  napi_get_boolean(env, host.IsRenderNodeEnabled(), &render_node);
  napi_set_named_property(env, settings, "enableRenderNode", render_node);

  return settings;
}

// saveThemePreferences(key, value) is saveToLocalStorage() with a different
// name on every platform; iOS additionally notifies the shell so open windows
// repaint. Here the card repaints itself from its own state, so persisting is
// all that is left to do.
napi_value SaveToLocalStorage(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string key;
  std::string value;
  if (!GetStringArgument(env, argc, argv, 0, &key) ||
      !GetStringArgument(env, argc, argv, 1, &value)) {
    return kUndefined;
  }
  ExplorerModuleHost::GetInstance().SaveToLocalStorage(key, value);
  return kUndefined;
}

napi_value ReadFromLocalStorage(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string key;
  if (!GetStringArgument(env, argc, argv, 0, &key)) {
    return kUndefined;
  }
  std::string value;
  if (!ExplorerModuleHost::GetInstance().ReadFromLocalStorage(key, &value)) {
    // The card checks `typeof value !== 'string'`, so undefined is the miss.
    return kUndefined;
  }
  napi_value result = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), value.size(), &result) !=
      napi_ok) {
    return kUndefined;
  }
  return result;
}

void ExportFunction(napi_env env, napi_value exports, const char* name,
                    napi_callback callback) {
  napi_value function = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, callback, nullptr,
                           &function) != napi_ok) {
    return;
  }
  napi_set_named_property(env, exports, name, function);
}

}  // namespace

napi_value ExplorerModuleCreator(napi_env env, napi_value exports,
                                 const char* module_name, void* opaque) {
  ExportFunction(env, exports, "openSchema", &OpenSchema);
  ExportFunction(env, exports, "openScan", &OpenScan);
  ExportFunction(env, exports, "setThreadMode", &SetThreadMode);
  ExportFunction(env, exports, "switchPreSize", &SwitchPreSize);
  ExportFunction(env, exports, "switchRenderNode", &SwitchRenderNode);
  ExportFunction(env, exports, "getSettingInfo", &GetSettingInfo);
  ExportFunction(env, exports, "saveThemePreferences", &SaveToLocalStorage);
  ExportFunction(env, exports, "saveToLocalStorage", &SaveToLocalStorage);
  ExportFunction(env, exports, "readFromLocalStorage", &ReadFromLocalStorage);
  // openRoute() and navigateBack() are deliberately absent. explorer/lib's
  // navigation.ts treats both as optional and only reaches for them when the
  // host sets the global prop explorerSupportsExplicitRouteOwnership, which
  // announces a coordinator that can own a route and hand it back. This
  // explorer has one window and no back stack, so it does not set that prop
  // and must not export the methods: exporting them would advertise a
  // capability that cannot be honoured.
  return exports;
}

}  // namespace example
}  // namespace lynx
