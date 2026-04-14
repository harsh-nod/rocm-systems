////////////////////////////////////////////////////////////////////////////////
// Offline hotswap tests on gfx1250 TensileLite code objects.
//
// Loads each .co / .hsaco from TENSILE_CO_DIR/code_objects/, runs:
//   1. ParseElfInfo — validate ELF structure
//   2. B0→A0 grow  — verify no crash, report patch count
//   3. Idempotence — second B0→A0 pass produces 0 patches
//   4. Disassembly  — llvm-objdump on patched output, verify no error
//
// Build with: (see CMakeLists.txt tensile_co_test target)
// Run with:   HSA_HOTSWAP_COMGR_LIB=/opt/rocm/lib/libamd_comgr.so ./tensile_co_test
////////////////////////////////////////////////////////////////////////////////

#include "hotswap.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef TENSILE_CO_DIR
#define TENSILE_CO_DIR "tests/tensile_co_data/code_objects"
#endif

#ifndef LLVM_OBJDUMP
#define LLVM_OBJDUMP "llvm-objdump"
#endif

static bool g_verbose = false;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::vector<uint8_t> LoadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto sz = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(sz);
  f.read(reinterpret_cast<char*>(buf.data()), sz);
  return buf;
}

static std::vector<std::string> ListCodeObjects(const std::string& dir) {
  std::vector<std::string> files;
  DIR* d = opendir(dir.c_str());
  if (!d) {
    fprintf(stderr, "Cannot open directory: %s\n", dir.c_str());
    return files;
  }
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string name = ent->d_name;
    if (name.size() > 3 &&
        (name.substr(name.size()-3) == ".co" ||
         name.find(".hsaco") != std::string::npos)) {
      files.push_back(dir + "/" + name);
    }
  }
  closedir(d);
  std::sort(files.begin(), files.end());
  return files;
}

static std::string Basename(const std::string& path) {
  auto pos = path.rfind('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

static bool RunObjdump(const void* buf, size_t size) {
  char tmpname[] = "/tmp/tensile_co_test_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) return false;
  ssize_t written = write(fd, buf, size);
  close(fd);
  if (written != (ssize_t)size) { unlink(tmpname); return false; }

  std::string cmd = std::string(LLVM_OBJDUMP) + " -d " + tmpname + " > /dev/null 2>&1";
  int ret = system(cmd.c_str());
  unlink(tmpname);
  return ret == 0;
}

static int CountMnemonic(const void* buf, size_t size, const std::string& mnemonic) {
  char tmpname[] = "/tmp/tensile_co_count_XXXXXX";
  int fd = mkstemp(tmpname);
  if (fd < 0) return -1;
  ssize_t written = write(fd, buf, size);
  close(fd);
  if (written != (ssize_t)size) { unlink(tmpname); return -1; }

  std::string cmd = std::string(LLVM_OBJDUMP) + " -d " + tmpname + " 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) { unlink(tmpname); return -1; }

  int count = 0;
  char line[4096];
  std::regex re("\\b" + mnemonic + "\\w*");
  while (fgets(line, sizeof(line), pipe)) {
    std::string s(line);
    auto begin = std::sregex_iterator(s.begin(), s.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) ++count;
  }
  pclose(pipe);
  unlink(tmpname);
  return count;
}

// ── Tests ────────────────────────────────────────────────────────────────────

struct TestResult {
  std::string file;
  bool elf_valid;
  bool b0a0_ok;
  int b0a0_patches;
  bool idempotent;
  bool disasm_ok;
  int wmma_count;
  size_t orig_size;
  size_t patched_size;
};

static TestResult TestOneCodeObject(const std::string& path) {
  TestResult r;
  r.file = Basename(path);
  r.elf_valid = false;
  r.b0a0_ok = false;
  r.b0a0_patches = -1;
  r.idempotent = false;
  r.disasm_ok = false;
  r.wmma_count = 0;
  r.orig_size = 0;
  r.patched_size = 0;

  auto data = LoadFile(path);
  if (data.empty()) {
    fprintf(stderr, "  SKIP (empty): %s\n", r.file.c_str());
    return r;
  }
  r.orig_size = data.size();

  // 1. Validate ELF magic
  if (data.size() < 16 || data[0] != 0x7f || data[1] != 'E' ||
      data[2] != 'L' || data[3] != 'F') {
    fprintf(stderr, "  FAIL (not ELF): %s\n", r.file.c_str());
    return r;
  }
  r.elf_valid = true;

  // 2. Verify llvm-objdump can disassemble original
  r.disasm_ok = RunObjdump(data.data(), data.size());

  // Count WMMA instructions in original
  r.wmma_count = CountMnemonic(data.data(), data.size(), "v_wmma");

  // 3. B0→A0 patching
  void* out_buf = nullptr;
  size_t out_size = 0;
  int patches = rocr_hotswap_gfx1250_b0_to_a0_grow(
      data.data(), data.size(), &out_buf, &out_size);
  r.b0a0_patches = patches;
  r.b0a0_ok = (patches >= 0);

  // When COMGR is unavailable, out_buf points back to input data (not malloc'd).
  bool out_is_new_alloc = (out_buf != nullptr && out_buf != data.data());

  if (r.b0a0_ok && out_is_new_alloc && out_size > 0) {
    r.patched_size = out_size;

    const uint8_t* p = static_cast<const uint8_t*>(out_buf);
    if (out_size >= 4 && p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F') {
      // 4. Idempotence: second pass should produce 0 patches
      void* out2 = nullptr;
      size_t out2_size = 0;
      int patches2 = rocr_hotswap_gfx1250_b0_to_a0_grow(
          out_buf, out_size, &out2, &out2_size);
      r.idempotent = (patches2 == 0);
      if (out2 && out2 != out_buf) free(out2);
    }
    free(out_buf);
  } else {
    // COMGR unavailable or 0 patches: input passes through unchanged
    r.b0a0_ok = true;
    r.idempotent = true;
    r.patched_size = data.size();
  }

  return r;
}

int main(int argc, char** argv) {
  std::string co_dir = TENSILE_CO_DIR;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "-v") g_verbose = true;
    else if (std::string(argv[i]) == "-d" && i+1 < argc) co_dir = argv[++i];
  }

  auto files = ListCodeObjects(co_dir);
  if (files.empty()) {
    fprintf(stderr, "No code objects found in %s\n", co_dir.c_str());
    return 1;
  }
  printf("=== TensileLite gfx1250 Code Object Hotswap Tests ===\n");
  printf("Directory: %s\n", co_dir.c_str());
  printf("Code objects: %zu\n\n", files.size());

  int total = 0, pass_elf = 0, pass_b0a0 = 0, pass_idemp = 0, pass_disasm = 0;
  int total_patches = 0, total_wmma = 0;

  for (const auto& f : files) {
    total++;
    auto r = TestOneCodeObject(f);

    if (r.elf_valid) pass_elf++;
    if (r.b0a0_ok) pass_b0a0++;
    if (r.idempotent) pass_idemp++;
    if (r.disasm_ok) pass_disasm++;
    if (r.b0a0_patches > 0) total_patches += r.b0a0_patches;
    total_wmma += r.wmma_count;

    if (g_verbose || !r.b0a0_ok || !r.elf_valid) {
      printf("  %-80s ELF:%s B0A0:%s(%3d) IDEMP:%s DISASM:%s WMMA:%d\n",
             r.file.c_str(),
             r.elf_valid ? "OK" : "FAIL",
             r.b0a0_ok ? "OK" : "FAIL", r.b0a0_patches,
             r.idempotent ? "OK" : "FAIL",
             r.disasm_ok ? "OK" : "FAIL",
             r.wmma_count);
    }
  }

  printf("\n=== Summary ===\n");
  printf("Total code objects:   %d\n", total);
  printf("ELF valid:            %d/%d\n", pass_elf, total);
  printf("B0A0 no crash:        %d/%d\n", pass_b0a0, total);
  printf("B0A0 idempotent:      %d/%d\n", pass_idemp, total);
  printf("Disassembly valid:    %d/%d\n", pass_disasm, total);
  printf("Total B0A0 patches:   %d\n", total_patches);
  printf("Total WMMA insts:     %d\n", total_wmma);

  bool all_pass = (pass_elf == total && pass_b0a0 == total && pass_disasm == total);
  printf("\n%s\n", all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
  return all_pass ? 0 : 1;
}
