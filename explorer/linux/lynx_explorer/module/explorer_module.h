// Copyright 2026 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.
#ifndef EXPLORER_LINUX_LYNX_EXPLORER_MODULE_EXPLORER_MODULE_H_
#define EXPLORER_LINUX_LYNX_EXPLORER_MODULE_EXPLORER_MODULE_H_

#include "platform/embedder/public/lynx_native_module.h"

#ifdef USE_WEAK_SUFFIX_NAPI
#include "third_party/weak-node-api/headers/weak_napi_defines.h"
#endif

namespace lynx {
namespace example {

// Builds the JS "ExplorerModule" object. Registered from main() with
// LynxEnv::RegisterNativeModule(), and called by the runtime the first time a
// card touches NativeModules.ExplorerModule.
napi_value ExplorerModuleCreator(napi_env env, napi_value exports,
                                 const char* module_name, void* opaque);

}  // namespace example
}  // namespace lynx

#ifdef USE_WEAK_SUFFIX_NAPI
#include "third_party/weak-node-api/headers/weak_napi_undefs.h"
#endif

#endif  // EXPLORER_LINUX_LYNX_EXPLORER_MODULE_EXPLORER_MODULE_H_
