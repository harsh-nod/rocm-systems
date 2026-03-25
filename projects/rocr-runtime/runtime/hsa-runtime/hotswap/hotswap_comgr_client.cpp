////////////////////////////////////////////////////////////////////////////////
//
// HotSwap COMGR Client — dlopen/dlsym binding implementation
//
////////////////////////////////////////////////////////////////////////////////

#include "hotswap_comgr_client.hpp"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <iostream>
#include <mutex>

namespace rocr {
namespace hotswap {

namespace {

struct ComgrHotswapResultC {
  int status;
  uint32_t rules_matched;
  uint32_t trampolines_added;
  uint32_t transpile_passthrough;
  uint32_t transpile_renamed;
  uint32_t transpile_waitcnt;
  uint32_t transpile_unsupported;
};

using RewriteFn = int (*)(
    const void*, size_t, const char*, const char*,
    uint32_t, const char*, void**, size_t*, ComgrHotswapResultC*);

using NeedsTranspileFn = int (*)(const char*, const char*, bool*);

struct ComgrBinding {
  void* handle = nullptr;
  RewriteFn rewrite = nullptr;
  NeedsTranspileFn needs_transpile = nullptr;
  bool attempted = false;
  bool available = false;
};

static std::once_flag g_init_flag;
static ComgrBinding g_binding;

static void InitBinding() {
  g_binding.attempted = true;

  const char* lib_path = std::getenv("HSA_HOTSWAP_COMGR_LIB");
  if (!lib_path || !*lib_path) {
    lib_path = "libamd_comgr.so";
  }

  g_binding.handle = dlopen(lib_path, RTLD_NOW | RTLD_LOCAL);
  if (!g_binding.handle) {
    const char* lib_path_v3 = "libamd_comgr.so.3";
    g_binding.handle = dlopen(lib_path_v3, RTLD_NOW | RTLD_LOCAL);
  }

  if (!g_binding.handle) {
    std::cerr << "hotswap: COMGR not available (dlopen failed: "
              << dlerror() << ")\n";
    return;
  }

  g_binding.rewrite = reinterpret_cast<RewriteFn>(
      dlsym(g_binding.handle, "amd_comgr_hotswap_rewrite"));
  g_binding.needs_transpile = reinterpret_cast<NeedsTranspileFn>(
      dlsym(g_binding.handle, "amd_comgr_hotswap_needs_transpile"));

  if (g_binding.rewrite && g_binding.needs_transpile) {
    g_binding.available = true;
    std::cerr << "hotswap: COMGR hotswap backend loaded successfully\n";
  } else {
    std::cerr << "hotswap: COMGR loaded but hotswap symbols not found"
              << " (needs COMGR >= 3.2)\n";
    dlclose(g_binding.handle);
    g_binding.handle = nullptr;
  }
}

static const ComgrBinding& GetBinding() {
  std::call_once(g_init_flag, InitBinding);
  return g_binding;
}

} // anonymous namespace

bool ComgrHotswapAvailable() {
  return GetBinding().available;
}

int ComgrHotswapRewrite(
    const void* elf_data, size_t elf_size,
    const char* source_isa, const char* target_isa,
    uint32_t flags, const char* rules_json,
    void** out_elf, size_t* out_elf_size,
    ComgrHotswapResult* result) {
  const auto& b = GetBinding();
  if (!b.available || !b.rewrite) {
    return -1;
  }

  ComgrHotswapResultC c_result;
  std::memset(&c_result, 0, sizeof(c_result));

  int status = b.rewrite(elf_data, elf_size, source_isa, target_isa,
                         flags, rules_json, out_elf, out_elf_size, &c_result);

  if (result) {
    result->status = c_result.status;
    result->rules_matched = c_result.rules_matched;
    result->trampolines_added = c_result.trampolines_added;
    result->transpile_passthrough = c_result.transpile_passthrough;
    result->transpile_renamed = c_result.transpile_renamed;
    result->transpile_waitcnt = c_result.transpile_waitcnt;
    result->transpile_unsupported = c_result.transpile_unsupported;
  }

  return status;
}

int ComgrHotswapNeedsTranspile(
    const char* source_isa, const char* target_isa,
    bool* needs_transpile) {
  const auto& b = GetBinding();
  if (!b.available || !b.needs_transpile) {
    return -1;
  }
  return b.needs_transpile(source_isa, target_isa, needs_transpile);
}

} // namespace hotswap
} // namespace rocr
