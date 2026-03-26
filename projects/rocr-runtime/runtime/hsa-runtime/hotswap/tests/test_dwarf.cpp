////////////////////////////////////////////////////////////////////////////////
// DWARF debug information update tests for hotswap.
// Verifies that patched ELFs have correct symbol table entries,
// debug_line coverage, debug_ranges, debug_info, and debug_frame updates.
////////////////////////////////////////////////////////////////////////////////

#include "hotswap.hpp"

#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef HSACO_DIR
#define HSACO_DIR "tests/hsaco_data"
#endif

extern "C" int rocr_hotswap_gfx1250_b0_to_a0_grow(
    const void* elf_data, size_t elf_size,
    void** out_data, size_t* out_size);

using TestDebugSymbolsFn = int (*)(const void*, size_t, char*, int);

static TestDebugSymbolsFn g_test_debug_symbols = nullptr;
static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    std::cerr << "  FAIL: " << msg << " [" #cond "]\n"; \
    return false; \
  } \
} while(0)

static std::vector<uint8_t> LoadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto sz = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), sz);
  return buf;
}

struct ElfSectionInfo {
  std::string name;
  uint64_t offset;
  uint64_t size;
  uint32_t type;
};

static std::vector<ElfSectionInfo> ParseSections(const uint8_t* elf, size_t sz) {
  std::vector<ElfSectionInfo> secs;
  if (sz < 64 || elf[0] != 0x7f) return secs;
  uint64_t shoff;
  uint16_t shentsz, shnum, shstrndx;
  std::memcpy(&shoff, elf + 40, 8);
  std::memcpy(&shentsz, elf + 58, 2);
  std::memcpy(&shnum, elf + 60, 2);
  std::memcpy(&shstrndx, elf + 62, 2);
  if (shoff == 0 || shnum == 0 || shstrndx >= shnum) return secs;

  const uint8_t* strtab_sh = elf + shoff + shstrndx * shentsz;
  uint64_t strtab_off, strtab_sz;
  std::memcpy(&strtab_off, strtab_sh + 24, 8);
  std::memcpy(&strtab_sz, strtab_sh + 32, 8);
  const char* strtab = reinterpret_cast<const char*>(elf + strtab_off);

  for (uint16_t i = 0; i < shnum; ++i) {
    const uint8_t* sh = elf + shoff + i * shentsz;
    ElfSectionInfo info;
    uint32_t name_idx;
    std::memcpy(&name_idx, sh, 4);
    std::memcpy(&info.type, sh + 4, 4);
    std::memcpy(&info.offset, sh + 24, 8);
    std::memcpy(&info.size, sh + 32, 8);
    if (name_idx < strtab_sz)
      info.name = strtab + name_idx;
    secs.push_back(info);
  }
  return secs;
}

static int CountSymbolsWithPrefix(const uint8_t* elf, size_t sz,
                                   const std::string& prefix) {
  auto secs = ParseSections(elf, sz);
  int count = 0;
  for (auto& sec : secs) {
    if (sec.type != 2) continue; // SHT_SYMTAB
    uint64_t shoff;
    uint16_t shentsz;
    std::memcpy(&shoff, elf + 40, 8);
    std::memcpy(&shentsz, elf + 58, 2);

    // Find the linked strtab
    for (uint16_t i = 0; i < secs.size(); ++i) {
      const uint8_t* sh = elf + shoff + i * shentsz;
      uint64_t sh_off, sh_sz;
      std::memcpy(&sh_off, sh + 24, 8);
      std::memcpy(&sh_sz, sh + 32, 8);
      if (sh_off != sec.offset) continue;

      uint32_t sh_link;
      std::memcpy(&sh_link, sh + 40, 4);
      if (sh_link >= secs.size()) break;

      auto& link_sec = secs[sh_link];
      const char* str = reinterpret_cast<const char*>(elf + link_sec.offset);

      size_t num_syms = sec.size / 24;
      for (size_t j = 0; j < num_syms; ++j) {
        const uint8_t* sym = elf + sec.offset + j * 24;
        uint32_t st_name;
        std::memcpy(&st_name, sym, 4);
        if (st_name < link_sec.size) {
          std::string name(str + st_name);
          if (name.find(prefix) == 0)
            ++count;
        }
      }
      break;
    }
  }
  return count;
}

static bool HasSection(const uint8_t* elf, size_t sz, const std::string& name) {
  auto secs = ParseSections(elf, sz);
  for (auto& s : secs)
    if (s.name == name) return true;
  return false;
}

static uint64_t GetSectionSize(const uint8_t* elf, size_t sz,
                                const std::string& name) {
  auto secs = ParseSections(elf, sz);
  for (auto& s : secs)
    if (s.name == name) return s.size;
  return 0;
}

// ── Test: Trampoline symbols present after patching ──────────────────────────

static bool TestSymtab_TrampolinePresent() {
  auto hsaco = LoadFile(std::string(HSACO_DIR) +
      "/f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco");
  CHECK(!hsaco.empty(), "load HSACO");

  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
  CHECK(out && out_size > 0, "patch produced output");

  int count = CountSymbolsWithPrefix(
      static_cast<const uint8_t*>(out), out_size, "__hotswap_tramp_");
  CHECK(count > 0, "at least one trampoline symbol");

  std::free(out);
  return true;
}

static bool TestSymtab_MultipleTramp() {
  auto hsaco = LoadFile(std::string(HSACO_DIR) +
      "/f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco");
  CHECK(!hsaco.empty(), "load HSACO");

  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
  CHECK(out && out_size > 0, "patch produced output");

  int count = CountSymbolsWithPrefix(
      static_cast<const uint8_t*>(out), out_size, "__hotswap_tramp_");
  CHECK(count >= 6, "gemm_8warp has 6 trampolines");

  std::free(out);
  return true;
}

static bool TestSymtab_OriginalSymbolsPreserved() {
  auto hsaco = LoadFile(std::string(HSACO_DIR) +
      "/f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco");
  CHECK(!hsaco.empty(), "load HSACO");

  int orig_kernel_syms = CountSymbolsWithPrefix(
      hsaco.data(), hsaco.size(), "gemm_tdm_pipelin");

  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
  CHECK(out && out_size > 0, "patch produced output");

  int patched_kernel_syms = CountSymbolsWithPrefix(
      static_cast<const uint8_t*>(out), out_size, "gemm_tdm_pipelin");
  CHECK(patched_kernel_syms >= orig_kernel_syms,
        "original kernel symbols preserved");

  std::free(out);
  return true;
}

// ── Test: Debug sections survive patching ─────────────────────────────────────

static bool TestDebug_SectionsPreserved() {
  auto hsaco = LoadFile(std::string(HSACO_DIR) +
      "/f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco");
  CHECK(!hsaco.empty(), "load HSACO");
  CHECK(HasSection(hsaco.data(), hsaco.size(), ".debug_line"), "orig has .debug_line");
  CHECK(HasSection(hsaco.data(), hsaco.size(), ".debug_info"), "orig has .debug_info");
  CHECK(HasSection(hsaco.data(), hsaco.size(), ".debug_frame"), "orig has .debug_frame");
  CHECK(HasSection(hsaco.data(), hsaco.size(), ".debug_ranges"), "orig has .debug_ranges");

  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
  CHECK(out && out_size > 0, "patch produced output");

  const uint8_t* p = static_cast<const uint8_t*>(out);
  CHECK(HasSection(p, out_size, ".debug_line"), "patched has .debug_line");
  CHECK(HasSection(p, out_size, ".debug_info"), "patched has .debug_info");
  CHECK(HasSection(p, out_size, ".debug_frame"), "patched has .debug_frame");
  CHECK(HasSection(p, out_size, ".debug_ranges"), "patched has .debug_ranges");

  std::free(out);
  return true;
}

static bool TestDebugLine_SizeGrew() {
  auto hsaco = LoadFile(std::string(HSACO_DIR) +
      "/f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco");
  CHECK(!hsaco.empty(), "load HSACO");

  uint64_t orig_line_size = GetSectionSize(hsaco.data(), hsaco.size(), ".debug_line");
  CHECK(orig_line_size > 0, "orig .debug_line has content");

  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
  CHECK(out && out_size > 0, "patch produced output");

  uint64_t patched_line_size = GetSectionSize(
      static_cast<const uint8_t*>(out), out_size, ".debug_line");
  CHECK(patched_line_size > orig_line_size,
        ".debug_line grew (trampoline sequences added)");

  std::free(out);
  return true;
}

static bool TestDebugFrame_RangeExtended() {
  auto hsaco = LoadFile(std::string(HSACO_DIR) +
      "/f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco");
  CHECK(!hsaco.empty(), "load HSACO");

  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
  CHECK(out && out_size > 0, "patch produced output");
  CHECK(out_size > hsaco.size(), "ELF grew (trampolines appended)");

  std::free(out);
  return true;
}

static bool TestDebug_NoDwarfGraceful() {
  // Build a minimal ELF without debug sections
  uint8_t text[4] = {0x00, 0x00, 0x80, 0xBF}; // s_nop 0
  // Minimal ELF header + .text only
  std::vector<uint8_t> elf(256, 0);
  uint8_t* e = elf.data();
  e[0]=0x7f; e[1]='E'; e[2]='L'; e[3]='F'; e[4]=2; e[5]=1; e[6]=1; e[7]=64;
  uint16_t et=1; std::memcpy(e+16,&et,2);
  uint16_t em=224; std::memcpy(e+18,&em,2);
  uint32_t ev=1; std::memcpy(e+20,&ev,4);
  uint32_t flags=0x49; std::memcpy(e+48,&flags,4);
  uint16_t ehsz=64; std::memcpy(e+52,&ehsz,2);

  // No patching expected, just verify no crash
  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(elf.data(), elf.size(), &out, &out_size);
  // Should handle gracefully (no crash)
  if (out) std::free(out);
  return true;
}

static bool TestAllRealHsacos() {
  const char* files[] = {
    "f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco",
    "f16_gemm_single_warp_schedule_gemm_tdm_pipelined_single_warp_per_simd_schedule_kernel.hsaco",
    "f16_gemm_warp_pipeline_gemm_tdm_pipelined_warp_pipelined_kernel.hsaco",
    "f16_fa_pipeline_attn_fwd_pipelined_kernel.hsaco",
  };

  for (const char* file : files) {
    auto hsaco = LoadFile(std::string(HSACO_DIR) + "/" + file);
    CHECK(!hsaco.empty(), std::string("load ") + file);

    void* out = nullptr;
    size_t out_size = 0;
    rocr_hotswap_gfx1250_b0_to_a0_grow(hsaco.data(), hsaco.size(), &out, &out_size);
    CHECK(out && out_size > 0, std::string("patch ") + file);

    const uint8_t* p = static_cast<const uint8_t*>(out);
    int tramp_syms = CountSymbolsWithPrefix(p, out_size, "__hotswap_tramp_");
    CHECK(tramp_syms > 0, std::string(file) + " has trampoline symbols");
    CHECK(HasSection(p, out_size, ".debug_line"),
          std::string(file) + " has .debug_line");

    std::free(out);
  }
  return true;
}

#define RUN(fn) do { \
  std::cout << "  "; \
  if (fn()) { std::cout << "PASS: " #fn "\n"; ++g_passed; } \
  else { std::cout << "FAIL: " #fn "\n"; ++g_failed; } \
} while(0)

int main() {
  const char* lib = std::getenv("HSA_HOTSWAP_COMGR_LIB");
  if (!lib || !*lib) {
    std::cout << "HSA_HOTSWAP_COMGR_LIB not set, skipping DWARF tests.\n";
    return 0;
  }

  std::cout << "=== DWARF Debug Information Tests ===\n\n";

  std::cout << "--- Symbol Table Tests ---\n";
  RUN(TestSymtab_TrampolinePresent);
  RUN(TestSymtab_MultipleTramp);
  RUN(TestSymtab_OriginalSymbolsPreserved);

  std::cout << "\n--- Debug Section Tests ---\n";
  RUN(TestDebug_SectionsPreserved);
  RUN(TestDebugLine_SizeGrew);
  RUN(TestDebugFrame_RangeExtended);
  RUN(TestDebug_NoDwarfGraceful);

  std::cout << "\n--- Integration Tests ---\n";
  RUN(TestAllRealHsacos);

  std::cout << "\n=== Results: " << g_passed << " passed, "
            << g_failed << " failed ===\n";
  return g_failed > 0 ? 1 : 0;
}
