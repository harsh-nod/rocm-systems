////////////////////////////////////////////////////////////////////////////////
// Integration tests for COMGR hotswap transpiler, retarget, and thread safety.
//
// Tests exercise amd_comgr_hotswap_rewrite and amd_comgr_hotswap_needs_transpile
// via dlopen, covering:
//   R1: Cross-family transpile (gfx1250 → gfx942) through COMGR
//   R5: Same-ISA retarget (B0→A0 path) through COMGR
//   R6: Thread-safety stress test
////////////////////////////////////////////////////////////////////////////////

#include "hotswap.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// C entry point for assembling instructions via ROCR hotswap
extern "C" int rocr_hotswap_assemble_inst(const char* asm_str,
                                           uint8_t* out_bytes, int max_bytes);

// ── COMGR function pointer types ─────────────────────────────────────────────

using RewriteFn = int (*)(const void*, size_t, const char*, const char*,
                          uint32_t, const char*, void**, size_t*, void*);
using NeedsTranspileFn = int (*)(const char*, const char*, bool*);

static RewriteFn g_rewrite;
static NeedsTranspileFn g_needs_transpile;

// Hotswap flag constants (mirror amd_comgr_hotswap_flags_t)
static constexpr uint32_t FLAG_B0_TO_A0  = 0x1;
static constexpr uint32_t FLAG_RETARGET  = 0x2;
static constexpr uint32_t FLAG_TRANSPILE = 0x4;

#ifndef LLVM_OBJDUMP
#define LLVM_OBJDUMP "llvm-objdump"
#endif

// ── Test infrastructure ──────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s\n        %s:%d\n", msg, __FILE__, __LINE__); \
    g_failed++; \
    return; \
  } \
} while (0)

#define CHECK_EQ(actual, expected, msg) do { \
  auto _a = (actual); auto _e = (expected); \
  if (_a != _e) { \
    fprintf(stderr, "  FAIL: %s (expected %d, got %d)\n        %s:%d\n", \
            msg, (int)_e, (int)_a, __FILE__, __LINE__); \
    g_failed++; \
    return; \
  } \
} while (0)

static void Pass(const char* name) {
  fprintf(stderr, "  PASS: %s\n", name);
  g_passed++;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::vector<uint8_t> Assemble(const char* asm_str) {
  uint8_t buf[64];
  int n = rocr_hotswap_assemble_inst(asm_str, buf, sizeof(buf));
  if (n <= 0) return {};
  return std::vector<uint8_t>(buf, buf + n);
}

static std::vector<uint8_t> BuildMinimalGfx1250Elf(
    const std::vector<uint8_t>& text) {
  const char shstrtab[] = "\0.text\0.shstrtab";
  const uint32_t shstrtab_size = sizeof(shstrtab);
  const uint32_t name_text = 1;
  const uint32_t name_shstrtab = 7;

  uint64_t text_off = 64;
  uint64_t text_sz = text.size();
  uint64_t shstrtab_off = text_off + text_sz;
  uint64_t shdr_off = (shstrtab_off + shstrtab_size + 7) & ~7ULL;
  uint64_t total = shdr_off + 3 * 64;

  std::vector<uint8_t> elf(total, 0);
  uint8_t* e = elf.data();

  e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
  e[4] = 2;    // ELFCLASS64
  e[5] = 1;    // ELFDATA2LSB
  e[6] = 1;    // EV_CURRENT
  e[7] = 64;   // ELFOSABI_AMDGPU_HSA
  uint16_t et = 1; std::memcpy(e + 16, &et, 2);
  uint16_t em = 224; std::memcpy(e + 18, &em, 2);
  uint32_t ev = 1; std::memcpy(e + 20, &ev, 4);
  std::memcpy(e + 40, &shdr_off, 8);
  uint32_t flags = 0x49; std::memcpy(e + 48, &flags, 4);
  uint16_t ehsize = 64; std::memcpy(e + 52, &ehsize, 2);
  uint16_t shentsize = 64; std::memcpy(e + 58, &shentsize, 2);
  uint16_t shnum = 3; std::memcpy(e + 60, &shnum, 2);
  uint16_t shstrndx = 2; std::memcpy(e + 62, &shstrndx, 2);

  std::memcpy(e + text_off, text.data(), text_sz);
  std::memcpy(e + shstrtab_off, shstrtab, shstrtab_size);

  uint8_t* sh1 = e + shdr_off + 64;
  uint32_t sh_name1 = name_text; std::memcpy(sh1 + 0, &sh_name1, 4);
  uint32_t sh_type1 = 1; std::memcpy(sh1 + 4, &sh_type1, 4);
  uint64_t sh_flags1 = 0x6; std::memcpy(sh1 + 8, &sh_flags1, 8);
  std::memcpy(sh1 + 24, &text_off, 8);
  std::memcpy(sh1 + 32, &text_sz, 8);

  uint8_t* sh2 = e + shdr_off + 128;
  uint32_t sh_name2 = name_shstrtab; std::memcpy(sh2 + 0, &sh_name2, 4);
  uint32_t sh_type2 = 3; std::memcpy(sh2 + 4, &sh_type2, 4);
  std::memcpy(sh2 + 24, &shstrtab_off, 8);
  uint64_t shstrtab_sz = shstrtab_size;
  std::memcpy(sh2 + 32, &shstrtab_sz, 8);

  return elf;
}

static std::vector<uint8_t> BuildTestText() {
  std::vector<uint8_t> text;
  const char* insts[] = {
    "s_nop 0",
    "v_mov_b32 v0, v1",
    "global_load_b32 v0, v0, s[0:1]",
    "s_wait_loadcnt 0",
    "s_endpgm",
  };
  for (const char* inst : insts) {
    auto bytes = Assemble(inst);
    if (bytes.empty()) {
      fprintf(stderr, "  assembly failed: %s\n", inst);
      return {};
    }
    text.insert(text.end(), bytes.begin(), bytes.end());
  }
  auto snop = Assemble("s_nop 0");
  for (int i = 0; i < 16; ++i)
    text.insert(text.end(), snop.begin(), snop.end());
  return text;
}

static bool CheckElfMagic(const void* data, size_t size) {
  if (size < 64) return false;
  auto elf = static_cast<const uint8_t*>(data);
  return elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F';
}

static std::string Disasm(const void* buf, size_t size) {
  char tmpname[] = "/tmp/transpiler_test_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) return "";
  write(fd, buf, size);
  close(fd);

  std::string cmd = std::string(LLVM_OBJDUMP) + " -d " + tmpname + " 2>&1";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) { unlink(tmpname); return ""; }

  std::string result;
  char line[4096];
  while (fgets(line, sizeof(line), pipe)) result += line;
  pclose(pipe);
  unlink(tmpname);
  return result;
}

// ── R1: NeedsTranspile Tests ─────────────────────────────────────────────────

static void TestNeedsTranspile_CrossFamily() {
  bool needs = false;
  int rc = g_needs_transpile("amdgcn-amd-amdhsa--gfx1250",
                             "amdgcn-amd-amdhsa--gfx942", &needs);
  CHECK(rc == 0, "needs_transpile call should succeed");
  CHECK(needs == true, "gfx1250→gfx942 should need transpile (cross-family)");
  Pass("TestNeedsTranspile_CrossFamily");
}

static void TestNeedsTranspile_SameFamily() {
  bool needs = true;
  int rc = g_needs_transpile("amdgcn-amd-amdhsa--gfx1250",
                             "amdgcn-amd-amdhsa--gfx1250", &needs);
  CHECK(rc == 0, "needs_transpile call should succeed");
  CHECK(needs == false, "gfx1250→gfx1250 should NOT need transpile (same ISA)");
  Pass("TestNeedsTranspile_SameFamily");
}

static void TestNeedsTranspile_SameFamily_GFX9() {
  bool needs = true;
  int rc = g_needs_transpile("amdgcn-amd-amdhsa--gfx950",
                             "amdgcn-amd-amdhsa--gfx942", &needs);
  CHECK(rc == 0, "needs_transpile call should succeed");
  CHECK(needs == false, "gfx950→gfx942 should NOT need transpile (same GFX9 family)");
  Pass("TestNeedsTranspile_SameFamily_GFX9");
}

// ── R1: Transpile Tests ──────────────────────────────────────────────────────

static void TestTranspile_BasicKernel() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  // Result struct: status, rules_matched, trampolines_added, 4 transpile stats
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx942",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  CHECK(rc == 0, "transpile rewrite call should succeed");
  CHECK(out != nullptr, "transpile output should be non-null");
  CHECK(out_size > 0, "transpile output should have non-zero size");

  free(out);
  Pass("TestTranspile_BasicKernel");
}

static void TestTranspile_OutputValid() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx942",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  CHECK(rc == 0, "transpile rewrite call should succeed");
  CHECK(CheckElfMagic(out, out_size), "transpile output should be valid ELF");

  std::string disasm = Disasm(out, out_size);
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find(".text") != std::string::npos,
        "transpile output should have .text section");

  free(out);
  Pass("TestTranspile_OutputValid");
}

static void TestTranspile_MnemonicRename() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx942",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  CHECK(rc == 0, "transpile call should succeed");
  CHECK(out != nullptr && out_size > 0, "transpile output valid");

  std::string disasm = Disasm(out, out_size);
  CHECK(disasm.find("global_load_dword") != std::string::npos,
        "global_load_b32 should become global_load_dword on GFX9");

  free(out);
  Pass("TestTranspile_MnemonicRename");
}

static void TestTranspile_WaitcntMerge() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx942",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  CHECK(rc == 0, "transpile call should succeed");
  CHECK(out != nullptr && out_size > 0, "transpile output valid");

  std::string disasm = Disasm(out, out_size);
  CHECK(disasm.find("s_waitcnt") != std::string::npos,
        "s_wait_loadcnt should become s_waitcnt vmcnt on GFX9");

  free(out);
  Pass("TestTranspile_WaitcntMerge");
}

// ── R5: Retarget Tests ───────────────────────────────────────────────────────

static void TestRetarget_SameIsaNoChange() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx1250",
                     FLAG_B0_TO_A0, nullptr,
                     &out, &out_size, result);
  CHECK(rc == 0, "B0→A0 rewrite call should succeed");
  CHECK(out != nullptr, "B0→A0 output should be non-null");
  CHECK(out_size > 0, "B0→A0 output should have non-zero size");
  CHECK(CheckElfMagic(out, out_size), "B0→A0 output should be valid ELF");

  free(out);
  Pass("TestRetarget_SameIsaNoChange");
}

static void TestRetarget_OutputValid() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx1250",
                     FLAG_B0_TO_A0, nullptr,
                     &out, &out_size, result);
  CHECK(rc == 0, "retarget call should succeed");
  CHECK(out != nullptr && out_size > 0, "retarget output valid");

  std::string disasm = Disasm(out, out_size);
  CHECK(!disasm.empty(), "retarget output should disassemble");
  CHECK(disasm.find(".text") != std::string::npos,
        "retarget output should have .text section");

  free(out);
  Pass("TestRetarget_OutputValid");
}

// ── R6: Thread-Safety Stress Test ────────────────────────────────────────────

static void TestThreadSafety_ConcurrentB0A0() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble gfx1250 test kernel for thread test");
  auto elf = BuildMinimalGfx1250Elf(text);

  constexpr int kNumThreads = 4;
  std::atomic<int> success_count{0};
  std::atomic<int> error_count{0};
  std::vector<std::thread> threads;

  for (int i = 0; i < kNumThreads; ++i) {
    threads.emplace_back([&]() {
      void* out = nullptr;
      size_t out_size = 0;
      int result[7] = {};

      int rc = g_rewrite(elf.data(), elf.size(),
                         "amdgcn-amd-amdhsa--gfx1250",
                         "amdgcn-amd-amdhsa--gfx1250",
                         FLAG_B0_TO_A0, nullptr,
                         &out, &out_size, result);

      if (rc == 0 && out != nullptr && out_size > 0 &&
          CheckElfMagic(out, out_size)) {
        success_count.fetch_add(1);
      } else {
        error_count.fetch_add(1);
      }
      free(out);
    });
  }

  for (auto& t : threads) t.join();

  CHECK(error_count.load() == 0, "no threads should have errors");
  CHECK_EQ(success_count.load(), kNumThreads,
           "all threads should produce valid output");
  Pass("TestThreadSafety_ConcurrentB0A0");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  const char* lib_path = getenv("HSA_HOTSWAP_COMGR_LIB");
  if (!lib_path) {
    fprintf(stderr, "HSA_HOTSWAP_COMGR_LIB not set, skipping transpiler tests\n");
    return 0;
  }

  void* handle = dlopen(lib_path, RTLD_LAZY);
  if (!handle) {
    fprintf(stderr, "Failed to dlopen %s: %s\n", lib_path, dlerror());
    fprintf(stderr, "Skipping transpiler tests\n");
    return 0;
  }

  g_rewrite = (RewriteFn)dlsym(handle, "amd_comgr_hotswap_rewrite");
  g_needs_transpile = (NeedsTranspileFn)dlsym(handle,
                                               "amd_comgr_hotswap_needs_transpile");

  if (!g_rewrite || !g_needs_transpile) {
    fprintf(stderr, "Failed to resolve COMGR hotswap symbols:\n");
    if (!g_rewrite)
      fprintf(stderr, "  missing: amd_comgr_hotswap_rewrite\n");
    if (!g_needs_transpile)
      fprintf(stderr, "  missing: amd_comgr_hotswap_needs_transpile\n");
    dlclose(handle);
    return 1;
  }

  fprintf(stderr, "=== Transpiler COMGR Integration Tests ===\n");

  fprintf(stderr, "\n--- NeedsTranspile Tests (R1) ---\n");
  TestNeedsTranspile_CrossFamily();
  TestNeedsTranspile_SameFamily();
  TestNeedsTranspile_SameFamily_GFX9();

  fprintf(stderr, "\n--- Transpile Tests (R1) ---\n");
  TestTranspile_BasicKernel();
  TestTranspile_OutputValid();
  TestTranspile_MnemonicRename();
  TestTranspile_WaitcntMerge();

  fprintf(stderr, "\n--- Retarget Tests (R5) ---\n");
  TestRetarget_SameIsaNoChange();
  TestRetarget_OutputValid();

  fprintf(stderr, "\n--- Thread-Safety Tests (R6) ---\n");
  TestThreadSafety_ConcurrentB0A0();

  fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n",
          g_passed, g_failed);

  dlclose(handle);
  return g_failed > 0 ? 1 : 0;
}
