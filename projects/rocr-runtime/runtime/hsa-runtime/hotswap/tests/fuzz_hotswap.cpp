////////////////////////////////////////////////////////////////////////////////
// Fuzz harness for hotswap ELF patching.
//
// Usage:
//   dd if=/dev/urandom bs=1024 count=10 | ./fuzz_hotswap -
//   ./fuzz_hotswap /path/to/input.bin
//
// With libFuzzer (if available):
//   clang++ -fsanitize=fuzzer,address -o fuzz_hotswap fuzz_hotswap.cpp ...
////////////////////////////////////////////////////////////////////////////////

#include "hotswap_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" int rocr_hotswap_gfx1250_b0_to_a0_grow(
    const void* elf_data, size_t elf_size,
    void** out_data, size_t* out_size);

static std::vector<uint8_t> ReadAll(FILE* f) {
  std::vector<uint8_t> buf;
  uint8_t chunk[4096];
  while (size_t n = fread(chunk, 1, sizeof(chunk), f))
    buf.insert(buf.end(), chunk, chunk + n);
  return buf;
}

static int RunOnInput(const uint8_t* data, size_t size) {
  // 1. Exercise ParseElfInfo — should never crash on arbitrary input
  rocr::hotswap::ElfInfo info;
  rocr::hotswap::ParseElfInfo(data, size, info);

  // 2. Exercise the full B0→A0 patching pipeline
  void* out = nullptr;
  size_t out_size = 0;
  rocr_hotswap_gfx1250_b0_to_a0_grow(data, size, &out, &out_size);
  if (out && out != static_cast<const void*>(data))
    std::free(out);

  return 0;
}

#ifdef __AFL_FUZZ_TESTCASE_LEN
// AFL++ persistent mode
__AFL_FUZZ_INIT();
int main() {
  __AFL_INIT();
  unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
  while (__AFL_LOOP(10000)) {
    int len = __AFL_FUZZ_TESTCASE_LEN;
    RunOnInput(buf, len);
  }
  return 0;
}
#elif defined(FUZZ_WITH_LIBFUZZER)
// libFuzzer entry point
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  return RunOnInput(data, size);
}
#else
// Standalone: read from file or stdin
int main(int argc, char** argv) {
  FILE* f = stdin;
  if (argc > 1 && strcmp(argv[1], "-") != 0) {
    f = fopen(argv[1], "rb");
    if (!f) {
      fprintf(stderr, "Cannot open %s\n", argv[1]);
      return 1;
    }
  }

  auto data = ReadAll(f);
  if (f != stdin) fclose(f);

  if (data.empty()) {
    fprintf(stderr, "fuzz_hotswap: empty input\n");
    return 0;
  }

  fprintf(stderr, "fuzz_hotswap: processing %zu bytes\n", data.size());
  RunOnInput(data.data(), data.size());
  fprintf(stderr, "fuzz_hotswap: OK (no crash)\n");
  return 0;
}
#endif
