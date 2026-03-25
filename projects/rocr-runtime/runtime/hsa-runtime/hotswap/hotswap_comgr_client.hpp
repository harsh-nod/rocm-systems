////////////////////////////////////////////////////////////////////////////////
//
// HotSwap COMGR Client — dynamic binding to amd_comgr hotswap APIs
//
// Uses dlopen/dlsym to load libamd_comgr.so at runtime and resolve the
// hotswap-specific entry points. Falls back gracefully if COMGR is not
// available or doesn't export the hotswap symbols.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef ROCR_HOTSWAP_COMGR_CLIENT_HPP
#define ROCR_HOTSWAP_COMGR_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace rocr {
namespace hotswap {

enum ComgrHotswapFlags : uint32_t {
  COMGR_HOTSWAP_FLAG_NONE          = 0x0,
  COMGR_HOTSWAP_FLAG_B0_TO_A0      = 0x1,
  COMGR_HOTSWAP_FLAG_RETARGET      = 0x2,
  COMGR_HOTSWAP_FLAG_TRANSPILE     = 0x4,
  COMGR_HOTSWAP_FLAG_REWRITE_RULES = 0x8,
};

struct ComgrHotswapResult {
  int status;
  uint32_t rules_matched;
  uint32_t trampolines_added;
  uint32_t transpile_passthrough;
  uint32_t transpile_renamed;
  uint32_t transpile_waitcnt;
  uint32_t transpile_unsupported;
};

bool ComgrHotswapAvailable();

int ComgrHotswapRewrite(
    const void* elf_data, size_t elf_size,
    const char* source_isa, const char* target_isa,
    uint32_t flags, const char* rules_json,
    void** out_elf, size_t* out_elf_size,
    ComgrHotswapResult* result);

int ComgrHotswapNeedsTranspile(
    const char* source_isa, const char* target_isa,
    bool* needs_transpile);

} // namespace hotswap
} // namespace rocr

#endif // ROCR_HOTSWAP_COMGR_CLIENT_HPP
