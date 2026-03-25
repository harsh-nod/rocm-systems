////////////////////////////////////////////////////////////////////////////////
//
// CLI tool: Apply gfx1250 B0→A0 binary patches to an .hsaco file.
//
// Usage:  b0a0_retarget_tool <input.hsaco> <output.hsaco>
//
// Reads the input code object, applies all three B0→A0 patches (CLUSTER_LOAD,
// DS 2-addr, s_clause), and writes the patched result.
//
////////////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int rocr_hotswap_gfx1250_b0_to_a0(void* elf_data, size_t elf_size);

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::fprintf(stderr, "Usage: %s <input.hsaco> <output.hsaco>\n",
                 argv[0]);
    return 1;
  }

  const char* input_path = argv[1];
  const char* output_path = argv[2];

  // Read input file
  FILE* fin = std::fopen(input_path, "rb");
  if (!fin) {
    std::fprintf(stderr, "Error: cannot open '%s' for reading\n", input_path);
    return 1;
  }
  std::fseek(fin, 0, SEEK_END);
  long file_size = std::ftell(fin);
  std::fseek(fin, 0, SEEK_SET);
  if (file_size <= 0) {
    std::fprintf(stderr, "Error: empty or unreadable file '%s'\n", input_path);
    std::fclose(fin);
    return 1;
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(file_size));
  size_t read = std::fread(buffer.data(), 1, buffer.size(), fin);
  std::fclose(fin);
  if (read != buffer.size()) {
    std::fprintf(stderr, "Error: short read on '%s'\n", input_path);
    return 1;
  }

  // Apply B0→A0 patches
  int patched = rocr_hotswap_gfx1250_b0_to_a0(buffer.data(), buffer.size());
  std::fprintf(stderr, "b0a0_retarget_tool: %d patches applied to '%s'\n",
               patched, input_path);

  // Write output file
  FILE* fout = std::fopen(output_path, "wb");
  if (!fout) {
    std::fprintf(stderr, "Error: cannot open '%s' for writing\n", output_path);
    return 1;
  }
  size_t written = std::fwrite(buffer.data(), 1, buffer.size(), fout);
  std::fclose(fout);
  if (written != buffer.size()) {
    std::fprintf(stderr, "Error: short write on '%s'\n", output_path);
    return 1;
  }

  std::printf("%d\n", patched);
  return 0;
}
