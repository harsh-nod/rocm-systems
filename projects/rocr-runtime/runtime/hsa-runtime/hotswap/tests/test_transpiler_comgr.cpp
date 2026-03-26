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

static int g_passed = 0, g_failed = 0, g_skipped = 0;

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

#define SKIP(reason) do { \
  fprintf(stderr, "  SKIP: %s\n", reason); \
  g_skipped++; \
  return; \
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

// ── Helper: Build text from arbitrary instruction list ────────────────────────

// Assembles a list of instructions into a .text blob, appending s_endpgm and
// padding nops. Returns empty vector if any instruction fails to assemble.
// If skip_on_fail is true, sets *skipped=true and returns empty on asm failure.
static std::vector<uint8_t> BuildTextFromInsts(
    const std::vector<const char*>& insts, bool* skipped = nullptr) {
  std::vector<uint8_t> text;
  for (const char* inst : insts) {
    auto bytes = Assemble(inst);
    if (bytes.empty()) {
      if (skipped) *skipped = true;
      fprintf(stderr, "  (assembly failed: %s)\n", inst);
      return {};
    }
    text.insert(text.end(), bytes.begin(), bytes.end());
  }
  auto endpgm = Assemble("s_endpgm");
  if (endpgm.empty()) return {};
  text.insert(text.end(), endpgm.begin(), endpgm.end());
  auto snop = Assemble("s_nop 0");
  for (int i = 0; i < 16; ++i)
    text.insert(text.end(), snop.begin(), snop.end());
  return text;
}

// Convenience: build ELF from instruction list, transpile gfx1250→gfx942,
// return disassembly string. Sets rc to the rewrite return code.
// Returns empty string if assembly or transpile fails.
static std::string TranspileAndDisasm(
    const std::vector<const char*>& insts, int* rc_out,
    bool* skipped = nullptr) {
  auto text = BuildTextFromInsts(insts, skipped);
  if (text.empty()) { if (rc_out) *rc_out = -1; return ""; }
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};
  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx942",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  if (rc_out) *rc_out = rc;
  if (rc != 0 || !out || out_size == 0) { free(out); return ""; }

  std::string disasm = Disasm(out, out_size);
  free(out);
  return disasm;
}

// ── Transpile: Memory Renames ────────────────────────────────────────────────

static void TestTranspile_DSLoadRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm({"ds_load_b32 v0, v1"}, &rc, &skipped);
  if (skipped) SKIP("ds_load_b32 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile ds_load_b32 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("ds_read_b32") != std::string::npos,
        "ds_load_b32 should become ds_read_b32 on GFX9");
  Pass("TestTranspile_DSLoadRename");
}

static void TestTranspile_DSStoreRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm({"ds_store_b32 v0, v1"}, &rc, &skipped);
  if (skipped) SKIP("ds_store_b32 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile ds_store_b32 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("ds_write_b32") != std::string::npos,
        "ds_store_b32 should become ds_write_b32 on GFX9");
  Pass("TestTranspile_DSStoreRename");
}

static void TestTranspile_ScratchLoadRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"scratch_load_b32 v0, v1, off"}, &rc, &skipped);
  if (skipped) SKIP("scratch_load_b32 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile scratch_load_b32 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("scratch_load_dword") != std::string::npos,
        "scratch_load_b32 should become scratch_load_dword on GFX9");
  Pass("TestTranspile_ScratchLoadRename");
}

static void TestTranspile_BufferLoadRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"buffer_load_b32 v0, v1, s[0:3], 0 offen"}, &rc, &skipped);
  if (skipped) SKIP("buffer_load_b32 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile buffer_load_b32 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("buffer_load_dword") != std::string::npos,
        "buffer_load_b32 should become buffer_load_dword on GFX9");
  Pass("TestTranspile_BufferLoadRename");
}

static void TestTranspile_SMEMLoadRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"s_load_b64 s[0:1], s[2:3], 0"}, &rc, &skipped);
  if (skipped) SKIP("s_load_b64 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile s_load_b64 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("s_load_dwordx2") != std::string::npos,
        "s_load_b64 should become s_load_dwordx2 on GFX9");
  Pass("TestTranspile_SMEMLoadRename");
}

// ── Transpile: Wait Counter ──────────────────────────────────────────────────

static void TestTranspile_WaitStorecnt() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"global_store_b32 v[0:1], v2, off", "s_wait_storecnt 0"}, &rc, &skipped);
  if (skipped) SKIP("s_wait_storecnt or global_store_b32 not supported for gfx1250");
  CHECK(rc == 0, "transpile with s_wait_storecnt should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("s_waitcnt") != std::string::npos,
        "s_wait_storecnt should become s_waitcnt on GFX9");
  Pass("TestTranspile_WaitStorecnt");
}

static void TestTranspile_WaitSamplecnt() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"s_nop 0", "s_wait_samplecnt 0"}, &rc, &skipped);
  if (skipped) SKIP("s_wait_samplecnt not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile with s_wait_samplecnt should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  Pass("TestTranspile_WaitSamplecnt");
}

static void TestTranspile_MultipleWaitCounters() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm({
    "global_load_b32 v0, v[0:1], off",
    "s_wait_loadcnt 0",
    "global_store_b32 v[0:1], v2, off",
    "s_wait_storecnt 0",
    "s_nop 0",
    "s_wait_kmcnt 0",
  }, &rc, &skipped);
  if (skipped) SKIP("one or more wait instructions not supported for gfx1250");
  CHECK(rc == 0, "transpile with multiple wait counters should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  // All wait counters should collapse to s_waitcnt on GFX9
  CHECK(disasm.find("s_waitcnt") != std::string::npos,
        "wait counters should become s_waitcnt on GFX9");
  Pass("TestTranspile_MultipleWaitCounters");
}

// ── Transpile: Control Flow ──────────────────────────────────────────────────

static void TestTranspile_ConditionalBranch() {
  bool skipped = false;
  int rc = 0;
  // s_cbranch_scc0 with offset 0 (branch to next instruction)
  auto disasm = TranspileAndDisasm(
      {"s_cbranch_scc0 0", "s_nop 0"}, &rc, &skipped);
  if (skipped) SKIP("s_cbranch_scc0 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile s_cbranch_scc0 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("s_cbranch_scc0") != std::string::npos,
        "s_cbranch_scc0 should survive transpile");
  Pass("TestTranspile_ConditionalBranch");
}

static void TestTranspile_UnconditionalBranch() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"s_branch 0", "s_nop 0"}, &rc, &skipped);
  if (skipped) SKIP("s_branch not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile s_branch should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("s_branch") != std::string::npos,
        "s_branch should survive transpile");
  Pass("TestTranspile_UnconditionalBranch");
}

// ── Transpile: WMMA/Matrix ───────────────────────────────────────────────────

static void TestTranspile_WMMA_F32() {
  bool skipped = false;
  int rc = 0;
  auto text = BuildTextFromInsts(
      {"v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]"},
      &skipped);
  if (skipped) SKIP("v_wmma_f32_16x16x64_fp8_fp8 not supported for gfx1250");
  CHECK(!text.empty(), "assemble WMMA instruction");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};
  rc = g_rewrite(elf.data(), elf.size(),
                 "amdgcn-amd-amdhsa--gfx1250",
                 "amdgcn-amd-amdhsa--gfx942",
                 FLAG_TRANSPILE, nullptr,
                 &out, &out_size, result);
  CHECK(rc == 0, "transpile WMMA instruction should succeed");
  CHECK(out != nullptr && out_size > 0, "transpile output valid");
  CHECK(CheckElfMagic(out, out_size), "transpile output is valid ELF");
  // WMMA may be lowered to MFMA or left as unsupported passthrough;
  // the key requirement is that the transpiler handles it without error.
  free(out);
  Pass("TestTranspile_WMMA_F32");
}

static void TestTranspile_WMMA_F16() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"v_wmma_f32_16x16x128_f8f6f4 v[0:7], v[8:23], v[24:39], v[0:7]"},
      &rc, &skipped);
  if (skipped) SKIP("v_wmma_f32_16x16x128_f8f6f4 not supported for gfx1250");
  CHECK(rc == 0, "transpile WMMA f8f6f4 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  Pass("TestTranspile_WMMA_F16");
}

// ── Transpile: EXEC/VCC Widening ─────────────────────────────────────────────

static void TestTranspile_ExecWidening() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"s_mov_b32 exec_lo, s0"}, &rc, &skipped);
  if (skipped) SKIP("s_mov_b32 exec_lo not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile exec_lo write should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  // GFX9 is wave64; exec_lo should be widened or rewritten
  bool has_exec = disasm.find("exec") != std::string::npos;
  CHECK(has_exec, "output should reference exec register");
  Pass("TestTranspile_ExecWidening");
}

static void TestTranspile_VCCUsage() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"v_cmp_eq_u32 vcc_lo, v0, v1"}, &rc, &skipped);
  if (skipped) SKIP("v_cmp_eq_u32 vcc_lo not supported for gfx1250");
  CHECK(rc == 0, "transpile VCC comparison should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  bool has_vcc = disasm.find("vcc") != std::string::npos;
  CHECK(has_vcc, "output should reference vcc register");
  Pass("TestTranspile_VCCUsage");
}

// ── Transpile: SALU ──────────────────────────────────────────────────────────

static void TestTranspile_SALUFloat() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"s_add_f32 s0, s0, s1"}, &rc, &skipped);
  if (skipped) SKIP("s_add_f32 not supported by llvm-mc for gfx1250");
  CHECK(rc == 0, "transpile SALU float should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  // SALU float may be lowered to VALU on GFX9. Check that something
  // was emitted, even if the exact lowering varies.
  Pass("TestTranspile_SALUFloat");
}

static void TestTranspile_SALUBitwise() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"s_and_b32 s0, s0, s1", "s_or_b32 s2, s2, s3"}, &rc, &skipped);
  if (skipped) SKIP("SALU bitwise instructions not supported for gfx1250");
  CHECK(rc == 0, "transpile SALU bitwise should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  // These should map 1:1 to GFX9
  bool has_and = disasm.find("s_and_b32") != std::string::npos;
  bool has_or = disasm.find("s_or_b32") != std::string::npos;
  CHECK(has_and, "s_and_b32 should survive on GFX9");
  CHECK(has_or, "s_or_b32 should survive on GFX9");
  Pass("TestTranspile_SALUBitwise");
}

// ── Transpile: Error Handling ────────────────────────────────────────────────

static void TestTranspile_FlagsZeroPassthrough() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  // flags=0 means no operation requested
  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "amdgcn-amd-amdhsa--gfx942",
                     0, nullptr,
                     &out, &out_size, result);
  // Depending on implementation, may succeed with passthrough or return error.
  // Either way, it should not crash.
  if (rc == 0 && out != nullptr) {
    CHECK(out_size > 0, "passthrough should produce output");
    free(out);
  }
  Pass("TestTranspile_FlagsZeroPassthrough");
}

static void TestTranspile_InvalidSourceISA() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx_INVALID",
                     "amdgcn-amd-amdhsa--gfx942",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  // Should either fail gracefully or produce passthrough; must not crash
  if (out) free(out);
  // We just verify we get here without crashing
  Pass("TestTranspile_InvalidSourceISA");
}

static void TestTranspile_InvalidTargetISA() {
  auto text = BuildTestText();
  CHECK(!text.empty(), "assemble test kernel");
  auto elf = BuildMinimalGfx1250Elf(text);

  void* out = nullptr;
  size_t out_size = 0;
  int result[7] = {};

  int rc = g_rewrite(elf.data(), elf.size(),
                     "amdgcn-amd-amdhsa--gfx1250",
                     "totally-bogus-isa-string",
                     FLAG_TRANSPILE, nullptr,
                     &out, &out_size, result);
  if (out) free(out);
  Pass("TestTranspile_InvalidTargetISA");
}

// ── Transpile: Complex Kernels ───────────────────────────────────────────────

static void TestTranspile_LoadStoreKernel() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm({
    "s_load_b64 s[0:1], s[2:3], 0",
    "s_wait_kmcnt 0",
    "global_load_b32 v0, v[0:1], off",
    "s_wait_loadcnt 0",
    "v_mov_b32 v2, v0",
    "global_store_b32 v[0:1], v2, off",
    "s_wait_storecnt 0",
  }, &rc, &skipped);
  if (skipped) SKIP("one or more instructions not supported for gfx1250");
  CHECK(rc == 0, "transpile load-store kernel should succeed");
  CHECK(!disasm.empty(), "output should disassemble");
  CHECK(disasm.find("global_load_dword") != std::string::npos,
        "global_load_b32 → global_load_dword");
  CHECK(disasm.find("global_store_dword") != std::string::npos,
        "global_store_b32 → global_store_dword");
  CHECK(disasm.find("s_load_dwordx2") != std::string::npos,
        "s_load_b64 → s_load_dwordx2");
  Pass("TestTranspile_LoadStoreKernel");
}

static void TestTranspile_SharedMemKernel() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm({
    "ds_load_b32 v0, v1",
    "s_wait_dscnt 0",
    "v_add_f32 v0, v0, v0",
    "ds_store_b32 v1, v0",
    "s_wait_dscnt 0",
  }, &rc, &skipped);
  if (skipped) SKIP("one or more DS instructions not supported for gfx1250");
  CHECK(rc == 0, "transpile shared-mem kernel should succeed");
  CHECK(!disasm.empty(), "output should disassemble");
  CHECK(disasm.find("ds_read_b32") != std::string::npos,
        "ds_load_b32 → ds_read_b32");
  CHECK(disasm.find("ds_write_b32") != std::string::npos,
        "ds_store_b32 → ds_write_b32");
  Pass("TestTranspile_SharedMemKernel");
}

static void TestTranspile_ALUHeavyKernel() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm({
    "v_mov_b32 v0, 0",
    "v_mov_b32 v1, 1",
    "v_add_f32 v2, v0, v1",
    "v_mul_f32 v3, v2, v1",
    "v_fma_f32 v4, v2, v3, v0",
    "s_and_b32 s0, s0, s1",
    "s_or_b32 s2, s2, s3",
    "v_cvt_f32_i32 v5, v0",
  }, &rc, &skipped);
  if (skipped) SKIP("one or more ALU instructions not supported for gfx1250");
  CHECK(rc == 0, "transpile ALU-heavy kernel should succeed");
  CHECK(!disasm.empty(), "output should disassemble");
  CHECK(disasm.find("v_add_f32") != std::string::npos,
        "v_add_f32 should survive on GFX9");
  CHECK(disasm.find("v_mul_f32") != std::string::npos,
        "v_mul_f32 should survive on GFX9");
  Pass("TestTranspile_ALUHeavyKernel");
}

// ── Additional coverage: global store, flat, and 64-bit ops ──────────────────

static void TestTranspile_GlobalStoreRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"global_store_b32 v[0:1], v2, off"}, &rc, &skipped);
  if (skipped) SKIP("global_store_b32 not supported for gfx1250");
  CHECK(rc == 0, "transpile global_store_b32 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("global_store_dword") != std::string::npos,
        "global_store_b32 should become global_store_dword on GFX9");
  Pass("TestTranspile_GlobalStoreRename");
}

static void TestTranspile_FlatLoadRename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"flat_load_b32 v0, v[0:1]"}, &rc, &skipped);
  if (skipped) SKIP("flat_load_b32 not supported for gfx1250");
  CHECK(rc == 0, "transpile flat_load_b32 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("flat_load_dword") != std::string::npos,
        "flat_load_b32 should become flat_load_dword on GFX9");
  Pass("TestTranspile_FlatLoadRename");
}

static void TestTranspile_GlobalLoad64Rename() {
  bool skipped = false;
  int rc = 0;
  auto disasm = TranspileAndDisasm(
      {"global_load_b64 v[0:1], v[2:3], off"}, &rc, &skipped);
  if (skipped) SKIP("global_load_b64 not supported for gfx1250");
  CHECK(rc == 0, "transpile global_load_b64 should succeed");
  CHECK(!disasm.empty(), "transpile output should disassemble");
  CHECK(disasm.find("global_load_dwordx2") != std::string::npos,
        "global_load_b64 should become global_load_dwordx2 on GFX9");
  Pass("TestTranspile_GlobalLoad64Rename");
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

  fprintf(stderr, "\n--- Transpile: Memory Renames ---\n");
  TestTranspile_DSLoadRename();
  TestTranspile_DSStoreRename();
  TestTranspile_ScratchLoadRename();
  TestTranspile_BufferLoadRename();
  TestTranspile_SMEMLoadRename();
  TestTranspile_GlobalStoreRename();
  TestTranspile_FlatLoadRename();
  TestTranspile_GlobalLoad64Rename();

  fprintf(stderr, "\n--- Transpile: Wait Counters ---\n");
  TestTranspile_WaitStorecnt();
  TestTranspile_WaitSamplecnt();
  TestTranspile_MultipleWaitCounters();

  fprintf(stderr, "\n--- Transpile: Control Flow ---\n");
  TestTranspile_ConditionalBranch();
  TestTranspile_UnconditionalBranch();

  fprintf(stderr, "\n--- Transpile: WMMA/Matrix ---\n");
  TestTranspile_WMMA_F32();
  TestTranspile_WMMA_F16();

  fprintf(stderr, "\n--- Transpile: EXEC/VCC ---\n");
  TestTranspile_ExecWidening();
  TestTranspile_VCCUsage();

  fprintf(stderr, "\n--- Transpile: SALU ---\n");
  TestTranspile_SALUFloat();
  TestTranspile_SALUBitwise();

  fprintf(stderr, "\n--- Transpile: Error Handling ---\n");
  TestTranspile_FlagsZeroPassthrough();
  TestTranspile_InvalidSourceISA();
  TestTranspile_InvalidTargetISA();

  fprintf(stderr, "\n--- Transpile: Complex Kernels ---\n");
  TestTranspile_LoadStoreKernel();
  TestTranspile_SharedMemKernel();
  TestTranspile_ALUHeavyKernel();

  fprintf(stderr, "\n--- Retarget Tests (R5) ---\n");
  TestRetarget_SameIsaNoChange();
  TestRetarget_OutputValid();

  fprintf(stderr, "\n--- Thread-Safety Tests (R6) ---\n");
  TestThreadSafety_ConcurrentB0A0();

  fprintf(stderr, "\n=== Results: %d passed, %d failed, %d skipped ===\n",
          g_passed, g_failed, g_skipped);

  dlclose(handle);
  return g_failed > 0 ? 1 : 0;
}
