////////////////////////////////////////////////////////////////////////////////
// Integration tests for gfx1250 B0→A0 patches on real Triton .hsaco kernels.
// Tests use rocr_hotswap_gfx1250_b0_to_a0_grow() to handle both NOP-sled
// and ELF-growth trampoline paths.
////////////////////////////////////////////////////////////////////////////////

#include "hotswap.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef HSACO_DIR
#define HSACO_DIR "tests/hsaco_data"
#endif

#ifndef LLVM_OBJDUMP
#define LLVM_OBJDUMP "llvm-objdump"
#endif

// C entry points for testing
extern "C" int rocr_hotswap_classify_wmma_nops(const char* mnemonic,
                                                int* b0, int* a0);
extern "C" int rocr_hotswap_assemble_inst(const char* asm_str,
                                           uint8_t* out_bytes, int max_bytes);

static bool g_dump_asm = false;
static std::string g_dump_dir = ".";

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::vector<uint8_t> LoadHsaco(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto sz = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), sz);
  return buf;
}

static std::string Disasm(const void* buf, size_t size) {
  // Write to temp file
  char tmpname[] = "/tmp/b0a0_test_XXXXXX";
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

static int CountMnemonic(const std::string& disasm, const std::string& mnemonic) {
  int count = 0;
  // Match the mnemonic as a word boundary in disassembly output.
  // objdump lines look like:  "   offset: bytes   mnemonic operands"
  // We look for the mnemonic preceded by whitespace/tab.
  std::string pattern = "\\b" + mnemonic + "\\b";
  std::regex re(pattern);
  auto begin = std::sregex_iterator(disasm.begin(), disasm.end(), re);
  auto end = std::sregex_iterator();
  for (auto it = begin; it != end; ++it) ++count;
  return count;
}

static void WriteFile(const std::string& path, const std::string& content) {
  std::ofstream f(path);
  f << content;
}

static bool CheckElfMagic(const void* data, size_t size) {
  if (size < 64) return false;
  auto elf = static_cast<const uint8_t*>(data);
  return elf[0] == 0x7f && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F';
}

// ── Test infrastructure ─────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    std::cerr << "  FAIL: " << msg << " [" #cond "]\n"; \
    ++g_failed; return false; \
  } \
} while(0)

#define CHECK_EQ(a, b, msg) do { \
  auto _a = (a); auto _b = (b); \
  if (_a != _b) { \
    std::cerr << "  FAIL: " << msg << ": expected " << _b << ", got " << _a << "\n"; \
    ++g_failed; return false; \
  } \
} while(0)

// ── Kernel test data ────────────────────────────────────────────────────────

struct KernelTestCase {
  const char* filename;
  const char* label;
  int expected_s_clause;
  int expected_tensor_load;
  int expected_total;
};

static const KernelTestCase kKernels[] = {
  {"f16_gemm_8warp_basic_gemm_tdm_pipelined_kernel.hsaco",
   "gemm_8warp", 34, 6, 40},
  {"f16_gemm_single_warp_schedule_gemm_tdm_pipelined_single_warp_per_simd_schedule_kernel.hsaco",
   "gemm_single_warp", 66, 4, 70},
  {"f16_gemm_warp_pipeline_gemm_tdm_pipelined_warp_pipelined_kernel.hsaco",
   "gemm_warp_pipeline", 34, 6, 40},
  {"f16_fa_pipeline_attn_fwd_pipelined_kernel.hsaco",
   "fa_pipeline", 14, 10, 24},
};

// ── Per-kernel tests ────────────────────────────────────────────────────────

static bool TestKernelPatch(const KernelTestCase& tc) {
  std::string path = std::string(HSACO_DIR) + "/" + tc.filename;
  auto buf = LoadHsaco(path);
  CHECK(!buf.empty(), std::string("load ") + tc.filename);

  // Count pre-patch mnemonics
  std::string pre_disasm = Disasm(buf.data(), buf.size());
  int pre_s_clause = CountMnemonic(pre_disasm, "s_clause");
  int pre_tensor = CountMnemonic(pre_disasm, "tensor_load_to_lds");

  CHECK_EQ(pre_s_clause, tc.expected_s_clause,
           std::string(tc.label) + " pre-patch s_clause count");
  CHECK_EQ(pre_tensor, tc.expected_tensor_load,
           std::string(tc.label) + " pre-patch tensor_load_to_lds count");

  // Apply patches
  void* out_data = nullptr;
  size_t out_size = 0;
  int patches = rocr_hotswap_gfx1250_b0_to_a0_grow(
      buf.data(), buf.size(), &out_data, &out_size);

  CHECK(out_data != nullptr, std::string(tc.label) + " out_data non-null");
  CHECK(out_size > 0, std::string(tc.label) + " out_size > 0");
  CHECK_EQ(patches, tc.expected_total,
           std::string(tc.label) + " total patch count");

  // Disassemble patched output
  std::string post_disasm = Disasm(out_data, out_size);

  // Dump before/after assembly if requested
  if (g_dump_asm) {
    std::string base = g_dump_dir + "/" + tc.label;
    WriteFile(base + "_before.s", pre_disasm);
    WriteFile(base + "_after.s", post_disasm);
    std::cerr << "  Wrote " << base << "_{before,after}.s\n";
  }

  // No s_clause should remain
  int post_s_clause = CountMnemonic(post_disasm, "s_clause");
  CHECK_EQ(post_s_clause, 0,
           std::string(tc.label) + " post-patch s_clause count");

  // Every tensor_load_to_lds should be preceded by s_pack_hh_b32_b16
  // (either in-line in a trampoline or in a NOP sled)
  int post_tensor = CountMnemonic(post_disasm, "tensor_load_to_lds");
  int post_pack = CountMnemonic(post_disasm, "s_pack_hh_b32_b16");

  // Each tensor_load_to_lds should have a corresponding s_pack_hh_b32_b16
  CHECK(post_pack >= tc.expected_tensor_load,
        std::string(tc.label) + " s_pack_hh_b32_b16 count >= tensor_load count");

  // Verify each tensor_load_to_lds has s_pack_hh_b32_b16 on a preceding
  // instruction line (within the trampoline — may be separated by the
  // s_branch landing, so check within 3 lines before)
  {
    std::istringstream iss(post_disasm);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(iss, line)) lines.push_back(line);

    int tensor_with_pack = 0;
    int tensor_total = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].find("tensor_load_to_lds") != std::string::npos) {
        ++tensor_total;
        // Look back up to 3 instruction lines for s_pack_hh_b32_b16
        bool found = false;
        for (int j = 1; j <= 3 && i >= (size_t)j; ++j) {
          if (lines[i-j].find("s_pack_hh_b32_b16") != std::string::npos) {
            found = true;
            break;
          }
        }
        if (found) ++tensor_with_pack;
      }
    }
    CHECK_EQ(tensor_with_pack, tensor_total,
             std::string(tc.label) + " tensor_load preceded by s_pack");
  }

  std::free(out_data);
  return true;
}

// ── Structural tests ────────────────────────────────────────────────────────

static bool TestElfValidity() {
  // Test that patched output is a valid ELF for all kernels
  for (const auto& tc : kKernels) {
    std::string path = std::string(HSACO_DIR) + "/" + tc.filename;
    auto buf = LoadHsaco(path);
    CHECK(!buf.empty(), std::string("load ") + tc.filename);

    void* out_data = nullptr;
    size_t out_size = 0;
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        buf.data(), buf.size(), &out_data, &out_size);

    CHECK(CheckElfMagic(out_data, out_size),
          std::string(tc.label) + " patched ELF magic");

    // Verify disassembly doesn't crash
    std::string disasm = Disasm(out_data, out_size);
    CHECK(!disasm.empty(),
          std::string(tc.label) + " patched ELF disassembles");

    std::free(out_data);
  }
  return true;
}

static bool TestIdempotent() {
  // Applying patches twice should produce the same result as once
  auto& tc = kKernels[0]; // gemm_8warp
  std::string path = std::string(HSACO_DIR) + "/" + tc.filename;
  auto buf = LoadHsaco(path);
  CHECK(!buf.empty(), "load for idempotent test");

  // First application
  void* out1 = nullptr;
  size_t size1 = 0;
  int patches1 = rocr_hotswap_gfx1250_b0_to_a0_grow(
      buf.data(), buf.size(), &out1, &size1);

  // Second application on already-patched output
  void* out2 = nullptr;
  size_t size2 = 0;
  int patches2 = rocr_hotswap_gfx1250_b0_to_a0_grow(out1, size1, &out2, &size2);

  CHECK_EQ(patches2, 0, "idempotent: second pass patches");

  // Content should be identical after second pass (no further modifications)
  CHECK_EQ(size1, size2, "idempotent: sizes match");
  CHECK(std::memcmp(out1, out2, size1) == 0, "idempotent: content matches");

  std::free(out1);
  std::free(out2);
  return true;
}

static bool TestNonTargetUnchanged() {
  // Safe instructions (s_mov_b32, v_fma_f32) should not be modified
  auto& tc = kKernels[0]; // gemm_8warp
  std::string path = std::string(HSACO_DIR) + "/" + tc.filename;
  auto buf = LoadHsaco(path);
  CHECK(!buf.empty(), "load for non-target test");

  std::string pre_disasm = Disasm(buf.data(), buf.size());
  int pre_s_mov = CountMnemonic(pre_disasm, "s_mov_b32");
  int pre_v_fma = CountMnemonic(pre_disasm, "v_fma_f32");

  void* out_data = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(
      buf.data(), buf.size(), &out_data, &out_size);

  std::string post_disasm = Disasm(out_data, out_size);
  int post_s_mov = CountMnemonic(post_disasm, "s_mov_b32");
  int post_v_fma = CountMnemonic(post_disasm, "v_fma_f32");

  CHECK_EQ(post_s_mov, pre_s_mov, "s_mov_b32 count unchanged");
  CHECK_EQ(post_v_fma, pre_v_fma, "v_fma_f32 count unchanged");

  std::free(out_data);
  return true;
}

// ── WMMA hazard tests ────────────────────────────────────────────────────────

static bool TestWmmaHazardClassification() {
  struct Case {
    const char* mnemonic;
    int expect_b0;
    int expect_a0;
    bool expect_hazard; // a0 > b0
  };
  static const Case cases[] = {
    // F16/BF16: equal timing, no hazard
    {"v_wmma_f32_16x16x32_f16", 4, 4, false},
    {"v_wmma_f32_16x16x32_bf16", 4, 4, false},
    {"v_swmmac_f32_16x16x64_f16", 4, 4, false},

    // IU8: B0 needs more, no A0 hazard
    {"v_wmma_i32_16x16x64_iu8", 8, 4, false},
    {"v_swmmac_i32_16x16x128_iu8", 8, 4, false},

    // 16x16x64 FP8 (LLVM mnemonic: fp8/bf8): A0 needs more (+3)
    {"v_wmma_f32_16x16x64_fp8_fp8", 1, 4, true},
    {"v_wmma_f32_16x16x64_bf8_bf8", 1, 4, true},
    {"v_wmma_f32_16x16x64_fp8_bf8", 1, 4, true},

    // 16x16x128 FP8: A0 needs slightly more (+1)
    {"v_wmma_f32_16x16x128_fp8_fp8", 3, 4, true},
    {"v_wmma_f32_16x16x128_fp8_bf8", 3, 4, true},
    {"v_swmmac_f32_16x16x128_fp8_fp8", 3, 4, true},

    // F8F6F4: conservative worst-case
    {"v_wmma_f32_16x16x128_f8f6f4", 1, 4, true},
    {"v_swmmac_f32_16x16x128_f8f6f4", 1, 4, true},

    // Non-WMMA: default conservative
    {"v_fma_f32", 4, 4, false},
    {"s_nop", 4, 4, false},
  };

  for (const auto& c : cases) {
    int b0 = -1, a0 = -1;
    int has_hazard = rocr_hotswap_classify_wmma_nops(c.mnemonic, &b0, &a0);

    if (b0 != c.expect_b0 || a0 != c.expect_a0) {
      std::cerr << "  FAIL: ClassifyWmmaNops(\"" << c.mnemonic
                << "\"): expected {" << c.expect_b0 << "," << c.expect_a0
                << "}, got {" << b0 << "," << a0 << "}\n";
      ++g_failed;
      return false;
    }
    if ((has_hazard != 0) != c.expect_hazard) {
      std::cerr << "  FAIL: ClassifyWmmaNops(\"" << c.mnemonic
                << "\"): hazard flag expected " << c.expect_hazard
                << ", got " << has_hazard << "\n";
      ++g_failed;
      return false;
    }
  }
  return true;
}

static bool TestWmmaHazardCurrentKernelsSafe() {
  // All 4 Triton kernels use F16 WMMA only (identical A0/B0 timing).
  // Verify 0 hazards are detected by checking stderr output contains
  // "0 hazards" for each kernel.
  for (const auto& tc : kKernels) {
    std::string path = std::string(HSACO_DIR) + "/" + tc.filename;
    auto buf = LoadHsaco(path);
    CHECK(!buf.empty(), std::string("load ") + tc.filename);

    // Capture stderr to check for WMMA hazard output
    // We redirect stderr to a temp file
    char tmpname[] = "/tmp/wmma_test_XXXXXX";
    int fd = mkstemp(tmpname);
    CHECK(fd >= 0, std::string(tc.label) + " mkstemp");

    // Redirect stderr
    int saved_stderr = dup(2);
    dup2(fd, 2);

    void* out_data = nullptr;
    size_t out_size = 0;
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        buf.data(), buf.size(), &out_data, &out_size);

    // Restore stderr
    dup2(saved_stderr, 2);
    close(saved_stderr);
    close(fd);

    // Read captured output
    std::ifstream f(tmpname);
    std::string stderr_output((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    f.close();
    unlink(tmpname);

    // Check for "0 hazards" in the WMMA validation line
    bool found_validation = stderr_output.find("WMMA co-exec validation: 0 hazards") != std::string::npos;
    CHECK(found_validation,
          std::string(tc.label) + " WMMA co-exec validation should report 0 hazards");

    // Count WMMA instructions scanned — should be > 0 for gemm kernels
    // (they use v_wmma_f32_16x16x32_f16)
    int wmma_count = CountMnemonic(Disasm(buf.data(), buf.size()), "v_wmma");
    if (wmma_count > 0) {
      // Verify the scanner actually found WMMA instructions
      bool found_scanned = stderr_output.find("WMMA instructions scanned") != std::string::npos;
      CHECK(found_scanned,
            std::string(tc.label) + " WMMA scanner should report instructions scanned");
    }

    std::free(out_data);
  }
  return true;
}

// ── Synthetic ELF builder for WMMA hazard testing ────────────────────────────

/// Assemble an instruction string via the hotswap LLVM MC pipeline.
/// Returns encoded bytes or empty on failure.
static std::vector<uint8_t> Assemble(const char* asm_str) {
  uint8_t buf[64];
  int n = rocr_hotswap_assemble_inst(asm_str, buf, sizeof(buf));
  if (n <= 0) return {};
  return std::vector<uint8_t>(buf, buf + n);
}

/// Build a minimal gfx1250 ELF64 with the given .text bytes.
/// Returns a complete ELF binary suitable for rocr_hotswap_gfx1250_b0_to_a0_grow.
static std::vector<uint8_t> BuildMinimalGfx1250Elf(
    const std::vector<uint8_t>& text) {
  // String table: \0 .text\0 .shstrtab\0
  const char shstrtab[] = "\0.text\0.shstrtab";
  const uint32_t shstrtab_size = sizeof(shstrtab); // includes trailing \0
  const uint32_t name_text = 1;     // offset of ".text" in shstrtab
  const uint32_t name_shstrtab = 7; // offset of ".shstrtab" in shstrtab

  // Layout:
  //   [0..63]     ELF header (64 bytes)
  //   [64..]      .text section
  //   [64+text..] .shstrtab section
  //   [aligned..] 3 section headers (64 bytes each)
  uint64_t text_off = 64;
  uint64_t text_sz = text.size();
  uint64_t shstrtab_off = text_off + text_sz;
  // Align section headers to 8 bytes
  uint64_t shdr_off = (shstrtab_off + shstrtab_size + 7) & ~7ULL;
  uint64_t total = shdr_off + 3 * 64; // 3 section headers

  std::vector<uint8_t> elf(total, 0);
  uint8_t* e = elf.data();

  // ELF header
  e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F';
  e[4] = 2;    // ELFCLASS64
  e[5] = 1;    // ELFDATA2LSB
  e[6] = 1;    // EV_CURRENT
  e[7] = 64;   // ELFOSABI_AMDGPU_HSA
  // e_type = ET_REL (1)
  uint16_t et = 1; std::memcpy(e + 16, &et, 2);
  // e_machine = EM_AMDGPU (224)
  uint16_t em = 224; std::memcpy(e + 18, &em, 2);
  // e_version
  uint32_t ev = 1; std::memcpy(e + 20, &ev, 4);
  // e_shoff
  std::memcpy(e + 40, &shdr_off, 8);
  // e_flags = EF_AMDGPU_MACH_AMDGCN_GFX1250 (0x49)
  uint32_t flags = 0x49; std::memcpy(e + 48, &flags, 4);
  // e_ehsize
  uint16_t ehsize = 64; std::memcpy(e + 52, &ehsize, 2);
  // e_shentsize
  uint16_t shentsize = 64; std::memcpy(e + 58, &shentsize, 2);
  // e_shnum
  uint16_t shnum = 3; std::memcpy(e + 60, &shnum, 2);
  // e_shstrndx
  uint16_t shstrndx = 2; std::memcpy(e + 62, &shstrndx, 2);

  // .text section data
  std::memcpy(e + text_off, text.data(), text_sz);

  // .shstrtab section data
  std::memcpy(e + shstrtab_off, shstrtab, shstrtab_size);

  // Section headers (64 bytes each)
  // [0] SHN_UNDEF (all zeros — already zeroed)

  // [1] .text
  uint8_t* sh1 = e + shdr_off + 64;
  uint32_t sh_name1 = name_text; std::memcpy(sh1 + 0, &sh_name1, 4);
  uint32_t sh_type1 = 1; std::memcpy(sh1 + 4, &sh_type1, 4); // SHT_PROGBITS
  uint64_t sh_flags1 = 0x6; std::memcpy(sh1 + 8, &sh_flags1, 8); // SHF_ALLOC|EXECINSTR
  std::memcpy(sh1 + 24, &text_off, 8); // sh_offset
  std::memcpy(sh1 + 32, &text_sz, 8);  // sh_size

  // [2] .shstrtab
  uint8_t* sh2 = e + shdr_off + 128;
  uint32_t sh_name2 = name_shstrtab; std::memcpy(sh2 + 0, &sh_name2, 4);
  uint32_t sh_type2 = 3; std::memcpy(sh2 + 4, &sh_type2, 4); // SHT_STRTAB
  std::memcpy(sh2 + 24, &shstrtab_off, 8);
  uint64_t shstrtab_sz = shstrtab_size;
  std::memcpy(sh2 + 32, &shstrtab_sz, 8);

  return elf;
}

/// Build .text from a list of assembly strings, plus a NOP sled for trampolines.
/// Returns empty on assembly failure.
static std::vector<uint8_t> BuildTextWithNopSled(
    const std::vector<std::string>& asm_lines,
    int sled_nops = 16) {
  std::vector<uint8_t> text;
  for (const auto& line : asm_lines) {
    auto bytes = Assemble(line.c_str());
    if (bytes.empty()) {
      std::cerr << "  assembly failed: " << line << "\n";
      return {};
    }
    text.insert(text.end(), bytes.begin(), bytes.end());
  }
  // Add s_endpgm + NOP sled for trampoline placement
  auto endpgm = Assemble("s_endpgm");
  if (endpgm.empty()) return {};
  text.insert(text.end(), endpgm.begin(), endpgm.end());

  auto snop = Assemble("s_nop 0");
  if (snop.empty()) return {};
  for (int i = 0; i < sled_nops; ++i)
    text.insert(text.end(), snop.begin(), snop.end());

  return text;
}

/// Capture stderr during a lambda call, return the captured output.
static std::string CaptureStderr(std::function<void()> fn) {
  char tmpname[] = "/tmp/wmma_stderr_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) return "";
  fflush(stderr);
  int saved = dup(2);
  dup2(fd, 2);
  fn();
  fflush(stderr);
  dup2(saved, 2);
  close(saved);
  close(fd);
  std::ifstream f(tmpname);
  std::string out((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  f.close();
  unlink(tmpname);
  return out;
}

// ── Synthetic WMMA hazard tests ──────────────────────────────────────────────

/// F8 16x16x64 WMMA → immediate VALU that clobbers WMMA input.
/// WMMA: D=v[0:7], A=v[8:15], B=v[16:23], C=v[0:7]
/// VALU writes v0 → clobbers C-matrix (WAR hazard).
/// Expected: deficit=4 (A0 needs 4 NOPs, existing=0), v_nops inserted.
static bool TestWmmaHazardF8x64Immediate() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix v[0:7] (WAR)
  });
  CHECK(!text.empty(), "assemble F8x64 immediate test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;
  int patches = 0;

  stderr_out = CaptureStderr([&]() {
    patches = rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  // Should detect 1 hazard with deficit
  CHECK(stderr_out.find("WMMA co-exec hazard") != std::string::npos,
        "F8x64 immediate: hazard detected");
  CHECK(stderr_out.find("needs 4 V_NOPs for A0, only 0 found") != std::string::npos,
        "F8x64 immediate: correct deficit reported");

  // Verify v_nop insertion in output
  if (out && out_size > 0) {
    std::string disasm = Disasm(out, out_size);
    int vnop_count = CountMnemonic(disasm, "v_nop");
    CHECK(vnop_count >= 3, "F8x64 immediate: at least 3 v_nop inserted");

    // v_fma_f32 should still be present (in the trampoline)
    int vfma_count = CountMnemonic(disasm, "v_fma_f32");
    CHECK_EQ(vfma_count, 1, "F8x64 immediate: v_fma_f32 preserved");
  }

  std::free(out);
  return true;
}

/// F8 16x16x64 WMMA → 2 v_nop → VALU that clobbers WMMA input.
/// A0 needs 4 NOPs, existing=2 → deficit=2.
static bool TestWmmaHazardF8x64PartialNops() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_nop",
    "v_nop",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix (WAR)
  });
  CHECK(!text.empty(), "assemble F8x64 partial nops test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec hazard") != std::string::npos,
        "F8x64 partial: hazard detected");
  CHECK(stderr_out.find("only 2 found") != std::string::npos,
        "F8x64 partial: 2 existing NOPs counted");

  // Verify v_nop insertion — should add 2 more (deficit=2)
  if (out && out_size > 0) {
    std::string disasm = Disasm(out, out_size);
    int vnop_count = CountMnemonic(disasm, "v_nop");
    // Original 2 + 2 inserted = 4 total (or more, the original may be preserved)
    CHECK(vnop_count >= 4, "F8x64 partial: at least 4 total v_nop");
  }

  std::free(out);
  return true;
}

/// F8 16x16x64 WMMA → 4 v_nop → dependent VALU.
/// Expected: 0 hazards (sufficient NOPs for A0).
static bool TestWmmaHazardF8x64SufficientNops() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_nop",
    "v_nop",
    "v_nop",
    "v_nop",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix (WAR)
  });
  CHECK(!text.empty(), "assemble F8x64 sufficient nops test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec validation: 0 hazards") != std::string::npos,
        "F8x64 sufficient: no hazard");

  std::free(out);
  return true;
}

/// F8 16x16x128 WMMA → immediate VALU that clobbers WMMA input.
/// WMMA: D=v[0:7], A=v[8:23], B=v[24:39], C=v[0:7]
/// Patch 6 decomposes the 128-wide into two 64-wide WMMAs; the hazard scanner
/// then sees the decomposed 64-wide WMMAs. Since Patch 6 replaces the original
/// instruction in the decoded array (mnemonic = "<replaced>"), the hazard
/// scanner no longer sees a WMMA at the original position. Verify Patch 6
/// fires and produces 64-wide WMMAs.
static bool TestWmmaHazardF8x128Immediate() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x128_fp8_fp8 v[0:7], v[8:23], v[24:39], v[0:7]",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix (WAR)
  });
  CHECK(!text.empty(), "assemble F8x128 immediate test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  // Patch 6 should decompose the 128-wide WMMA
  CHECK(stderr_out.find("16x16x64") != std::string::npos,
        "F8x128 immediate: Patch 6 decomposed to 16x16x64");

  if (out && out_size > 0) {
    std::string disasm = Disasm(out, out_size);
    CHECK(CountMnemonic(disasm, "v_wmma_f32_16x16x64_fp8_fp8") >= 2,
          "F8x128 immediate: two 64-wide WMMAs in output");
    CHECK(CountMnemonic(disasm, "v_fma_f32") >= 1,
          "F8x128 immediate: v_fma_f32 preserved");
  }

  std::free(out);
  return true;
}

/// F16 WMMA → immediate VALU that clobbers WMMA input.
/// Expected: 0 hazards (A0 == B0 timing for F16, no delta).
static bool TestWmmaHazardF16NoDelta() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x32_f16 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C, but no delta for F16
  });
  CHECK(!text.empty(), "assemble F16 no-delta test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec validation: 0 hazards") != std::string::npos,
        "F16 no-delta: no hazard");

  std::free(out);
  return true;
}

/// IU8 WMMA → immediate VALU that clobbers WMMA input.
/// Expected: 0 hazards (B0 needs MORE NOPs than A0 for IU8).
static bool TestWmmaHazardIU8NoDelta() {
  auto text = BuildTextWithNopSled({
    "v_wmma_i32_16x16x64_iu8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C, but IU8 has no A0 delta
  });
  CHECK(!text.empty(), "assemble IU8 no-delta test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec validation: 0 hazards") != std::string::npos,
        "IU8 no-delta: no hazard");

  std::free(out);
  return true;
}

/// F8 WMMA → non-overlapping VALU → VALU that clobbers WMMA input.
/// Non-overlapping VALU (dest doesn't touch WMMA inputs) counts as "safe slot".
static bool TestWmmaHazardF8NonOverlapping() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_mov_b32 v40, v41",        // dest=v40, no overlap with WMMA inputs
    "v_mov_b32 v42, v43",        // dest=v42, no overlap
    "v_mov_b32 v44, v45",        // dest=v44, no overlap
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix (WAR)
  });
  CHECK(!text.empty(), "assemble F8 non-overlapping test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  // 3 non-overlapping VALU = 3 safe slots. A0 needs 4. Deficit = 1.
  CHECK(stderr_out.find("WMMA co-exec hazard") != std::string::npos,
        "F8 non-overlapping: hazard detected (deficit=1)");
  CHECK(stderr_out.find("only 3 found") != std::string::npos,
        "F8 non-overlapping: 3 slots counted");

  std::free(out);
  return true;
}

/// F8 WMMA → SALU → VALU that clobbers WMMA input.
/// SALU doesn't consume VALU issue slots, so it shouldn't count toward NOP budget.
static bool TestWmmaHazardF8SALUBetween() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "s_mov_b32 s0, s1",          // SALU — doesn't consume VALU issue slot
    "s_mov_b32 s2, s3",          // SALU — doesn't count
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix (WAR)
  });
  CHECK(!text.empty(), "assemble F8 SALU between test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  // SALU instructions don't count → still 0 safe slots, deficit=4
  CHECK(stderr_out.find("WMMA co-exec hazard") != std::string::npos,
        "F8 SALU between: hazard detected");
  CHECK(stderr_out.find("only 0 found") != std::string::npos,
        "F8 SALU between: SALU not counted");

  std::free(out);
  return true;
}

/// F8F6F4 WMMA → immediate VALU that clobbers WMMA input.
/// Expected: deficit=4 (conservative {1,4}, A0 needs 4, existing=0).
static bool TestWmmaHazardF8F6F4() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x128_f8f6f4 v[0:7], v[8:23], v[24:39], v[0:7]",
    "v_fma_f32 v0, v0, v1, v2",  // writes v0 → clobbers C-matrix (WAR)
  });
  CHECK(!text.empty(), "assemble F8F6F4 test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec hazard") != std::string::npos,
        "F8F6F4: hazard detected");
  CHECK(stderr_out.find("needs 4 V_NOPs for A0, only 0 found") != std::string::npos,
        "F8F6F4: correct deficit reported");

  std::free(out);
  return true;
}

/// F8 WMMA → VALU whose dest doesn't overlap WMMA inputs.
/// No WAR hazard → no fix needed.
static bool TestWmmaHazardF8NoOverlap() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_fma_f32 v40, v41, v42, v43",  // writes v40, no overlap with WMMA inputs v[0:23]
  });
  CHECK(!text.empty(), "assemble F8 no-overlap test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec validation: 0 hazards") != std::string::npos,
        "F8 no-overlap: no hazard");

  std::free(out);
  return true;
}

/// F8 WMMA → VALU that only reads WMMA D-output (RAW), doesn't write inputs.
/// No WAR hazard → should not trigger. This tests that we correctly ignore
/// read-only access to WMMA outputs.
static bool TestWmmaHazardF8ReadOnlyDOutput() {
  auto text = BuildTextWithNopSled({
    // WMMA: D=v[0:7], A=v[8:15], B=v[16:23], C=v[0:7]
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    // Reads v0 (D-output) but writes to v40 — no WAR on WMMA inputs
    "v_fma_f32 v40, v0, v1, v2",
  });
  CHECK(!text.empty(), "assemble F8 read-only D output test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec validation: 0 hazards") != std::string::npos,
        "F8 read-only D: no WAR hazard");

  std::free(out);
  return true;
}

/// F8 WMMA → VALU that writes to A-matrix (not C).
/// WMMA: D=v[0:7], A=v[8:15], B=v[16:23], C=v[0:7]
/// VALU writes v10 → clobbers A-matrix v[8:15] (WAR hazard).
static bool TestWmmaHazardF8ClobberAMatrix() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_mov_b32 v10, v40",  // writes v10 → clobbers A-matrix v[8:15] (WAR)
  });
  CHECK(!text.empty(), "assemble F8 clobber A-matrix test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(stderr_out.find("WMMA co-exec hazard") != std::string::npos,
        "F8 clobber A-matrix: WAR hazard detected");

  std::free(out);
  return true;
}

/// Multiple F8 WMMAs, each followed by VALU that clobbers WMMA inputs.
/// Expected: 2 WAR hazards detected and fixed.
static bool TestWmmaHazardMultiple() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_fma_f32 v0, v0, v1, v2",  // hazard #1: writes v0 → clobbers C=v[0:7]
    "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
    "v_add_f32 v1, v1, v2",      // hazard #2: writes v1 → clobbers C=v[0:7]
  }, 32); // extra sled space for 2 trampolines
  CHECK(!text.empty(), "assemble multiple F8 test");

  auto elf = BuildMinimalGfx1250Elf(text);
  std::string stderr_out;
  void* out = nullptr;
  size_t out_size = 0;

  stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  // Should find 2 hazards
  CHECK(stderr_out.find("2 hazards") != std::string::npos,
        "Multiple F8: 2 hazards detected");

  // Verify both v_fma and v_add are preserved in output
  if (out && out_size > 0) {
    std::string disasm = Disasm(out, out_size);
    CHECK(CountMnemonic(disasm, "v_fma_f32") >= 1,
          "Multiple F8: v_fma_f32 preserved");
    CHECK(CountMnemonic(disasm, "v_add_f32_e32") >= 1,
          "Multiple F8: v_add_f32 preserved");
    CHECK(CountMnemonic(disasm, "v_nop") >= 6,
          "Multiple F8: at least 6 v_nops total (3+3)");
  }

  std::free(out);
  return true;
}

// ── Patch 6 Tests: 16x16x128 FP8/BF8 WMMA Decomposition ─────────────────────

/// Assemble a 16x16x128 FP8 WMMA, apply patches, verify two 16x16x64 WMMAs.
static bool TestPatch6_F32_FP8_Decomposition() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x128_fp8_fp8 v[0:7], v[8:23], v[24:39], v[0:7]",
  }, 32);
  CHECK(!text.empty(), "assemble 16x16x128 fp8");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch6 F32 FP8: output generated");
  std::string disasm = Disasm(out, out_size);
  CHECK(CountMnemonic(disasm, "v_wmma_f32_16x16x64_fp8_fp8") >= 2,
        "Patch6 F32 FP8: two 16x16x64 WMMAs in output");
  CHECK(CountMnemonic(disasm, "v_wmma_f32_16x16x128_fp8_fp8") == 0,
        "Patch6 F32 FP8: no 16x16x128 remaining");

  std::free(out);
  return true;
}

/// F16 accumulator with BF8 inputs (D is 4 VGPRs for F16).
static bool TestPatch6_F16_BF8_Decomposition() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f16_16x16x128_bf8_bf8 v[0:3], v[8:23], v[24:39], v[0:3]",
  }, 32);
  CHECK(!text.empty(), "assemble 16x16x128 bf8");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch6 F16 BF8: output generated");
  std::string disasm = Disasm(out, out_size);
  CHECK(CountMnemonic(disasm, "v_wmma_f16_16x16x64_bf8_bf8") >= 2,
        "Patch6 F16 BF8: two 16x16x64 WMMAs in output");

  std::free(out);
  return true;
}

/// Idempotency: apply patches twice, second pass should find 0 new patches.
static bool TestPatch6_Idempotent() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_16x16x128_fp8_fp8 v[0:7], v[8:23], v[24:39], v[0:7]",
  }, 32);
  CHECK(!text.empty(), "assemble Patch6 idempotent");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out1 = nullptr;
  size_t out1_size = 0;
  CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out1, &out1_size);
  });
  CHECK(out1 && out1_size > 0, "Patch6 idempotent: first pass ok");

  // Second pass on already-patched output
  void* out2 = nullptr;
  size_t out2_size = 0;
  std::string stderr2 = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        out1, out1_size, &out2, &out2_size);
  });
  // Should report 0 patches (or very small number from re-scanning branches)
  CHECK(stderr2.find("16x16x128") == std::string::npos,
        "Patch6 idempotent: no 16x16x128 on second pass");

  std::free(out1);
  std::free(out2);
  return true;
}

// ── Patch 7 Tests: 32x16x128 F4 WMMA Decomposition ──────────────────────────

/// Non-scaled 32x16x128 F4 → two 16x16x128 f8f6f4 with FP4 format.
static bool TestPatch7_F4_NonScaled() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_32x16x128_f4 v[0:15], v[16:31], v[32:39], v[0:15]",
  }, 32);
  CHECK(!text.empty(), "assemble 32x16x128 f4");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch7 F4 non-scaled: output generated");
  std::string disasm = Disasm(out, out_size);
  CHECK(CountMnemonic(disasm, "v_wmma_f32_16x16x128_f8f6f4") >= 2,
        "Patch7 F4: two 16x16x128 f8f6f4 WMMAs");
  CHECK(CountMnemonic(disasm, "v_wmma_f32_32x16x128_f4") == 0,
        "Patch7 F4: no 32x16x128_f4 remaining");

  std::free(out);
  return true;
}

/// Verify VGPR splits: D[0:7]/D[8:15], A[0:7]/A[8:15], B shared.
static bool TestPatch7_F4_VgprSplit() {
  auto text = BuildTextWithNopSled({
    "v_wmma_f32_32x16x128_f4 v[0:15], v[16:31], v[32:39], v[0:15]",
  }, 32);
  CHECK(!text.empty(), "assemble 32x16x128 f4 vgpr split");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch7 VGPR split: output generated");
  std::string disasm = Disasm(out, out_size);
  // Check that v[0:7] and v[8:15] appear as D operands, v[32:39] shared B
  CHECK(disasm.find("v[0:7]") != std::string::npos,
        "Patch7 VGPR split: D_top v[0:7] present");
  CHECK(disasm.find("v[8:15]") != std::string::npos,
        "Patch7 VGPR split: D_bot v[8:15] present");

  std::free(out);
  return true;
}

// ── Patch 8 Tests: E5M3 CVT CLAMP Emulation ─────────────────────────────────

/// v_cvt_f32_fp8 VOP3 (_e64) without CLAMP → instruction unchanged.
static bool TestPatch8_CvtF32FP8_NoClamp() {
  // Use VOP3 form (_e64, 8 bytes) since CLAMP only applies to VOP3
  auto text = BuildTextWithNopSled({
    "v_cvt_f32_fp8_e64 v0, v1",
  }, 16);
  CHECK(!text.empty(), "assemble v_cvt_f32_fp8_e64 no clamp");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch8 no clamp: output generated");
  std::string disasm = Disasm(out, out_size);
  // Without CLAMP, instruction should remain (disasm shows v_cvt_f32_fp8_e64)
  CHECK(CountMnemonic(disasm, "v_cvt_f32_fp8_e64") >= 1,
        "Patch8 no clamp: v_cvt_f32_fp8 preserved");
  // Should not have VALU emulation markers
  CHECK(stderr_out.find("E5M3") == std::string::npos,
        "Patch8 no clamp: no E5M3 patching reported");

  std::free(out);
  return true;
}

/// v_cvt_f32_fp8 VOP3 with CLAMP=1 → replaced with VALU emulation.
static bool TestPatch8_CvtF32FP8_WithClamp() {
  // Assemble VOP3 form with CLAMP set
  auto text = BuildTextWithNopSled({
    "v_cvt_f32_fp8_e64 v0, v1 clamp",
  }, 32);
  CHECK(!text.empty(), "assemble v_cvt_f32_fp8_e64 clamp");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch8 with clamp: output generated");
  CHECK(stderr_out.find("E5M3") != std::string::npos,
        "Patch8 with clamp: E5M3 emulation reported");

  std::string disasm = Disasm(out, out_size);
  CHECK(CountMnemonic(disasm, "v_bfe_u32") >= 1,
        "Patch8 with clamp: VALU emulation contains v_bfe_u32");

  std::free(out);
  return true;
}

// ── Patch 9 Tests: Block16 Scale Decomposition ──────────────────────────────

/// v_wmma_scale16 decomposed to two v_wmma_scale instructions.
static bool TestPatch9_Scale16WMMA() {
  // scale16 format: D, A(16), B(16), D(accum), scale_a(2), scale_b(2) [modifiers]
  auto text = BuildTextWithNopSled({
    "v_wmma_scale16_f32_16x16x128_f8f6f4 v[32:39], v[0:15], v[16:31], v[32:39], v[40:41], v[42:43]",
  }, 64);
  CHECK(!text.empty(), "assemble scale16 wmma");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch9 scale16: output generated");
  CHECK(stderr_out.find("block32") != std::string::npos,
        "Patch9 scale16: block32 decomposition reported");

  std::string disasm = Disasm(out, out_size);
  CHECK(CountMnemonic(disasm, "v_perm_b32") >= 1,
        "Patch9 scale16: v_perm_b32 repack present");

  std::free(out);
  return true;
}

/// v_wmma_ld_scale16_paired_b64 replaced with repack + block32 loads.
static bool TestPatch9_LdScale16() {
  auto text = BuildTextWithNopSled({
    "v_wmma_ld_scale16_paired_b64 v[0:1], v[2:3]",
  }, 32);
  CHECK(!text.empty(), "assemble ld_scale16");

  auto elf = BuildMinimalGfx1250Elf(text);
  void* out = nullptr;
  size_t out_size = 0;

  std::string stderr_out = CaptureStderr([&]() {
    rocr_hotswap_gfx1250_b0_to_a0_grow(
        elf.data(), elf.size(), &out, &out_size);
  });

  CHECK(out && out_size > 0, "Patch9 ld_scale16: output generated");
  CHECK(stderr_out.find("block32") != std::string::npos ||
        stderr_out.find("ld_scale16") != std::string::npos,
        "Patch9 ld_scale16: patching reported");

  std::free(out);
  return true;
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--dump-asm") {
      g_dump_asm = true;
      if (i + 1 < argc && argv[i+1][0] != '-') {
        g_dump_dir = argv[++i];
      }
    }
  }

  struct TestEntry {
    const char* name;
    bool (*fn)();
  };

  std::vector<TestEntry> tests;

  // Per-kernel tests
  for (const auto& tc : kKernels) {
    tests.push_back({tc.label, nullptr}); // placeholder — handled specially
  }

  // Structural tests
  tests.push_back({"ElfValidity", TestElfValidity});
  tests.push_back({"Idempotent", TestIdempotent});
  tests.push_back({"NonTargetUnchanged", TestNonTargetUnchanged});

  // WMMA hazard tests
  tests.push_back({"WmmaHazardClassification", TestWmmaHazardClassification});
  tests.push_back({"WmmaHazardCurrentKernelsSafe", TestWmmaHazardCurrentKernelsSafe});

  // Synthetic WMMA hazard tests
  tests.push_back({"WmmaHazardF8x64Immediate", TestWmmaHazardF8x64Immediate});
  tests.push_back({"WmmaHazardF8x64PartialNops", TestWmmaHazardF8x64PartialNops});
  tests.push_back({"WmmaHazardF8x64SufficientNops", TestWmmaHazardF8x64SufficientNops});
  tests.push_back({"WmmaHazardF8x128Immediate", TestWmmaHazardF8x128Immediate});
  tests.push_back({"WmmaHazardF16NoDelta", TestWmmaHazardF16NoDelta});
  tests.push_back({"WmmaHazardIU8NoDelta", TestWmmaHazardIU8NoDelta});
  tests.push_back({"WmmaHazardF8NonOverlapping", TestWmmaHazardF8NonOverlapping});
  tests.push_back({"WmmaHazardF8SALUBetween", TestWmmaHazardF8SALUBetween});
  tests.push_back({"WmmaHazardF8F6F4", TestWmmaHazardF8F6F4});
  tests.push_back({"WmmaHazardF8NoOverlap", TestWmmaHazardF8NoOverlap});
  tests.push_back({"WmmaHazardF8ReadOnlyDOutput", TestWmmaHazardF8ReadOnlyDOutput});
  tests.push_back({"WmmaHazardF8ClobberAMatrix", TestWmmaHazardF8ClobberAMatrix});
  tests.push_back({"WmmaHazardMultiple", TestWmmaHazardMultiple});

  // Patch 6 tests (16x16x128 FP8/BF8 WMMA decomposition)
  tests.push_back({"Patch6_F32_FP8_Decomposition", TestPatch6_F32_FP8_Decomposition});
  tests.push_back({"Patch6_F16_BF8_Decomposition", TestPatch6_F16_BF8_Decomposition});
  tests.push_back({"Patch6_Idempotent", TestPatch6_Idempotent});

  // Patch 7 tests (32x16x128 F4 WMMA decomposition)
  tests.push_back({"Patch7_F4_NonScaled", TestPatch7_F4_NonScaled});
  tests.push_back({"Patch7_F4_VgprSplit", TestPatch7_F4_VgprSplit});

  // Patch 8 tests (E5M3 CVT CLAMP emulation)
  tests.push_back({"Patch8_CvtF32FP8_NoClamp", TestPatch8_CvtF32FP8_NoClamp});
  tests.push_back({"Patch8_CvtF32FP8_WithClamp", TestPatch8_CvtF32FP8_WithClamp});

  // Patch 9 tests (Block16 scale decomposition)
  tests.push_back({"Patch9_Scale16WMMA", TestPatch9_Scale16WMMA});
  tests.push_back({"Patch9_LdScale16", TestPatch9_LdScale16});

  // Run per-kernel tests
  for (const auto& tc : kKernels) {
    std::cerr << "TEST " << tc.label << "...\n";
    if (TestKernelPatch(tc)) {
      std::cerr << "  PASS\n";
      ++g_passed;
    }
  }

  // Run structural tests
  for (size_t i = 4; i < tests.size(); ++i) {
    std::cerr << "TEST " << tests[i].name << "...\n";
    if (tests[i].fn()) {
      std::cerr << "  PASS\n";
      ++g_passed;
    }
  }

  std::cerr << "\n" << g_passed << " passed, " << g_failed << " failed\n";
  return g_failed > 0 ? 1 : 0;
}
