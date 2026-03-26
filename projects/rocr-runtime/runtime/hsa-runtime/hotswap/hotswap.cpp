////////////////////////////////////////////////////////////////////////////////
//
// The University of Illinois/NCSA
// Open Source License (NCSA)
//
// Copyright (c) 2014-2025, Advanced Micro Devices, Inc. All rights reserved.
//
// Developed by:
//
//                 AMD Research and AMD HSA Software Development
//
//                 Advanced Micro Devices, Inc.
//
//                 www.amd.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal with the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
//  - Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimers.
//  - Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimers in
//    the documentation and/or other materials provided with the distribution.
//  - Neither the names of Advanced Micro Devices, Inc,
//    nor the names of its contributors may be used to endorse or promote
//    products derived from this Software without specific prior written
//    permission.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE CONTRIBUTORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS WITH THE SOFTWARE.
//
////////////////////////////////////////////////////////////////////////////////

#include "hotswap.hpp"
#include "hotswap_core.hpp"
#include "hotswap_comgr_client.hpp"
#include "hotswap_rules.hpp"
#include "transpiler.hpp"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace rocr {
namespace hotswap {

namespace {

static std::string ReadRulesJson() {
  const char* path = std::getenv("HSA_HOTSWAP_RULES");
  if (!path || !*path) return {};
  std::ifstream ifs(path);
  if (!ifs) return {};
  return std::string(std::istreambuf_iterator<char>(ifs),
                     std::istreambuf_iterator<char>());
}

} // anonymous namespace

// ── Public API ───────────────────────────────────────────────────────────────

bool IsEnabled() {
  const char* rules = std::getenv("HSA_HOTSWAP_RULES");
  const char* override_isa = std::getenv("HSA_HOTSWAP_ISA_OVERRIDE");
  // Enabled if either rules file is set OR ISA override is set
  return (rules && *rules) || (override_isa && *override_isa && override_isa[0] != '0');
}

bool IsIsaOverrideEnabled() {
  const char* env = std::getenv("HSA_HOTSWAP_ISA_OVERRIDE");
  return env && *env && env[0] != '0';
}

bool PatchElfIsa(void* elf_data, size_t elf_size,
                 const std::string& target_isa) {
  uint8_t* elf = static_cast<uint8_t*>(elf_data);
  if (elf_size < 64) return false;
  if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
    return false;
  if (elf[4] != 2) return false; // Must be 64-bit

  // Extract the target gfx number for e_flags patching.
  // e_flags for AMDGPU ELF encodes the EF_AMDGPU_MACH value.
  // We need to map gfx names to their EF_AMDGPU_MACH constants.
  std::string target_cpu = ExtractCPU(target_isa);
  if (target_cpu.empty()) return false;

  // Map of gfx name → EF_AMDGPU_MACH value (from llvm/include/llvm/Support/ScopedPrinter.h)
  // and llvm/include/llvm/BinaryFormat/ELF.h
  struct GfxMach { const char* name; uint32_t mach; };
  static const GfxMach gfx_mach_map[] = {
    {"gfx900",  0x02c}, {"gfx902",  0x02d}, {"gfx904",  0x02e},
    {"gfx906",  0x02f}, {"gfx908",  0x030}, {"gfx909",  0x031},
    {"gfx90a",  0x03f}, {"gfx90c",  0x032}, {"gfx940",  0x04a},
    {"gfx941",  0x04b}, {"gfx942",  0x04c}, {"gfx950",  0x04f},
    {"gfx1010", 0x033}, {"gfx1011", 0x034}, {"gfx1012", 0x035},
    {"gfx1030", 0x036}, {"gfx1031", 0x037}, {"gfx1032", 0x038},
    {"gfx1033", 0x039}, {"gfx1034", 0x03e}, {"gfx1035", 0x03d},
    {"gfx1100", 0x041}, {"gfx1101", 0x046}, {"gfx1102", 0x047},
    {"gfx1103", 0x044}, {"gfx1150", 0x043}, {"gfx1151", 0x04b},
    {"gfx1200", 0x048}, {"gfx1201", 0x04a},
    {"gfx1250", 0x049}, {"gfx1251", 0x05a},
    {nullptr, 0}
  };

  uint32_t target_mach = 0;
  for (auto* p = gfx_mach_map; p->name; ++p) {
    if (target_cpu == p->name) { target_mach = p->mach; break; }
  }
  if (target_mach == 0) {
    std::cerr << "hotswap: unknown target CPU '" << target_cpu
              << "' for ISA override\n";
    return false;
  }

  // Patch e_flags: the MACH value is in bits [7:0] of e_flags
  // EF_AMDGPU_MACH mask = 0xFF
  uint32_t e_flags;
  std::memcpy(&e_flags, elf + 48, 4);
  e_flags = (e_flags & ~0xFFu) | (target_mach & 0xFF);
  std::memcpy(elf + 48, &e_flags, 4);

  // Also patch .note sections that contain the ISA name string.
  // AMDGPU code objects have NT_AMDGPU_ISA notes (type 27) with the ISA
  // string. We need to find and replace them.
  uint64_t e_shoff;
  uint16_t e_shentsize, e_shnum, e_shstrndx;
  std::memcpy(&e_shoff, elf + 40, 8);
  std::memcpy(&e_shentsize, elf + 58, 2);
  std::memcpy(&e_shnum, elf + 60, 2);
  std::memcpy(&e_shstrndx, elf + 62, 2);

  if (e_shoff == 0 || e_shnum == 0) return true; // e_flags patched, no notes

  for (uint16_t i = 0; i < e_shnum; ++i) {
    const uint8_t* sh = elf + e_shoff + i * e_shentsize;
    uint32_t sh_type;
    std::memcpy(&sh_type, sh + 4, 4);

    if (sh_type != 7 /* SHT_NOTE */) continue;

    uint64_t sh_offset, sh_size;
    std::memcpy(&sh_offset, sh + 24, 8);
    std::memcpy(&sh_size, sh + 32, 8);

    if (sh_offset + sh_size > elf_size) continue;

    // Walk through notes in this section
    uint64_t pos = sh_offset;
    while (pos + 12 <= sh_offset + sh_size) {
      uint32_t namesz, descsz, type;
      std::memcpy(&namesz, elf + pos, 4);
      std::memcpy(&descsz, elf + pos + 4, 4);
      std::memcpy(&type, elf + pos + 8, 4);

      uint32_t namesz_aligned = (namesz + 3) & ~3u;
      uint32_t descsz_aligned = (descsz + 3) & ~3u;
      uint64_t note_total = 12 + namesz_aligned + descsz_aligned;

      if (pos + note_total > sh_offset + sh_size) break;

      // NT_AMDGPU_ISA = 27, owner = "AMDGPU"
      if (type == 27 && namesz > 0) {
        const char* owner = reinterpret_cast<const char*>(elf + pos + 12);
        if (std::strncmp(owner, "AMDGPU", 6) == 0) {
          // The desc contains the ISA string (null-terminated)
          uint8_t* desc = elf + pos + 12 + namesz_aligned;
          std::string orig_isa(reinterpret_cast<const char*>(desc), descsz);

          // Replace gfx part of the ISA string in-place if it fits
          size_t gfx_pos = orig_isa.find("gfx");
          if (gfx_pos != std::string::npos) {
            // Find end of gfx token
            size_t gfx_end = gfx_pos;
            while (gfx_end < orig_isa.size() &&
                   orig_isa[gfx_end] != ':' && orig_isa[gfx_end] != '\0')
              ++gfx_end;
            std::string orig_gfx = orig_isa.substr(gfx_pos, gfx_end - gfx_pos);

            if (target_cpu.size() <= orig_gfx.size()) {
              // Fits in-place — overwrite with target CPU, pad with nulls
              std::memcpy(desc + gfx_pos, target_cpu.c_str(), target_cpu.size());
              for (size_t j = target_cpu.size(); j < orig_gfx.size(); ++j) {
                desc[gfx_pos + j] = '\0';
              }
              std::cerr << "hotswap: ISA override patched note: "
                        << orig_gfx << " -> " << target_cpu << "\n";
            } else {
              std::cerr << "hotswap: ISA override note patch failed: "
                        << target_cpu << " longer than " << orig_gfx << "\n";
            }
          }
        }
      }

      pos += note_total;
    }
  }

  return true;
}

// ── COMGR-backed rewrite functions ───────────────────────────────────────────

RewriteResult RetargetCodeObject(void* elf_data, size_t elf_size,
                                 const std::string& source_isa,
                                 const std::string& target_isa) {
  RewriteResult result = {HSA_STATUS_SUCCESS, 0, 0};

  if (!ComgrHotswapAvailable()) {
    std::cerr << "hotswap: COMGR not available for retarget\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  void* out_elf = nullptr;
  size_t out_size = 0;
  ComgrHotswapResult comgr_result = {};

  int rc = ComgrHotswapRewrite(
      elf_data, elf_size,
      source_isa.c_str(), target_isa.c_str(),
      COMGR_HOTSWAP_FLAG_RETARGET, nullptr,
      &out_elf, &out_size, &comgr_result);

  if (rc != 0) {
    std::cerr << "hotswap: COMGR retarget failed (rc=" << rc << ")\n";
    result.status = HSA_STATUS_ERROR;
    if (out_elf) std::free(out_elf);
    return result;
  }

  if (out_elf && out_elf != elf_data) {
    if (out_size <= elf_size) {
      std::memcpy(elf_data, out_elf, out_size);
    } else {
      std::cerr << "hotswap: retarget output larger than input ("
                << out_size << " > " << elf_size
                << "); use grow variant\n";
      result.status = HSA_STATUS_ERROR;
    }
    std::free(out_elf);
  }

  result.rules_matched = comgr_result.rules_matched;
  result.trampolines_added = comgr_result.trampolines_added;
  return result;
}

RewriteResult RewriteCodeObject(void* elf_data, size_t elf_size,
                                const std::string& isa_name) {
  void* out_data = nullptr;
  size_t out_size = 0;
  auto result = RewriteCodeObjectGrow(elf_data, elf_size, &out_data, &out_size,
                                      isa_name);

  if (out_data && out_data != elf_data) {
    std::cerr << "hotswap: RewriteCodeObject cannot grow buffer; use "
                 "RewriteCodeObjectGrow instead\n";
    std::free(out_data);
    result.status = HSA_STATUS_ERROR;
  }

  return result;
}

RewriteResult RewriteCodeObjectGrow(const void* elf_data, size_t elf_size,
                                    void** out_data, size_t* out_size,
                                    const std::string& isa_name) {
  RewriteResult result = {HSA_STATUS_SUCCESS, 0, 0};
  *out_data = const_cast<void*>(elf_data);
  *out_size = elf_size;

  const RulesFile* rules = GetCachedRules();
  if (!rules || rules->rules.empty()) return result;

  if (!rules->target.empty() && rules->target != isa_name) {
    return result;
  }

  if (!ComgrHotswapAvailable()) {
    std::cerr << "hotswap: COMGR not available for rewrite\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  std::string rules_json = ReadRulesJson();
  if (rules_json.empty()) {
    std::cerr << "hotswap: could not read rules file\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  void* out_elf = nullptr;
  size_t out_elf_size = 0;
  ComgrHotswapResult comgr_result = {};

  int rc = ComgrHotswapRewrite(
      elf_data, elf_size,
      isa_name.c_str(), isa_name.c_str(),
      COMGR_HOTSWAP_FLAG_REWRITE_RULES, rules_json.c_str(),
      &out_elf, &out_elf_size, &comgr_result);

  if (rc != 0) {
    std::cerr << "hotswap: COMGR rewrite failed (rc=" << rc << ")\n";
    result.status = HSA_STATUS_ERROR;
    if (out_elf) std::free(out_elf);
    return result;
  }

  if (out_elf) {
    *out_data = out_elf;
    *out_size = out_elf_size;
  }

  result.rules_matched = comgr_result.rules_matched;
  result.trampolines_added = comgr_result.trampolines_added;
  return result;
}

RewriteResult RetargetCodeObjectB0A0Grow(const void* elf_data, size_t elf_size,
                                         void** out_data, size_t* out_size) {
  RewriteResult result = {HSA_STATUS_SUCCESS, 0, 0};
  *out_data = const_cast<void*>(elf_data);
  *out_size = elf_size;

  const std::string isa = "amdgcn-amd-amdhsa--gfx1250";

  if (!ComgrHotswapAvailable()) {
    std::cerr << "hotswap: COMGR not available for B0->A0\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  void* out_elf = nullptr;
  size_t out_elf_size = 0;
  ComgrHotswapResult comgr_result = {};

  int rc = ComgrHotswapRewrite(
      elf_data, elf_size,
      isa.c_str(), isa.c_str(),
      COMGR_HOTSWAP_FLAG_B0_TO_A0, nullptr,
      &out_elf, &out_elf_size, &comgr_result);

  if (rc != 0) {
    std::cerr << "hotswap: COMGR B0->A0 rewrite failed (rc=" << rc << ")\n";
    result.status = HSA_STATUS_ERROR;
    if (out_elf) std::free(out_elf);
    return result;
  }

  if (out_elf) {
    *out_data = out_elf;
    *out_size = out_elf_size;
  }

  result.rules_matched = comgr_result.rules_matched;
  result.trampolines_added = comgr_result.trampolines_added;

  if (result.rules_matched > 0)
    std::cerr << "hotswap: gfx1250 B0->A0 grow (COMGR): "
              << result.rules_matched << " patches applied, "
              << result.trampolines_added << " trampolines\n";
  return result;
}

// ── Transpiler (COMGR-backed) ────────────────────────────────────────────────

bool NeedsTranspile(const std::string& source_isa,
                    const std::string& target_isa) {
  if (!ComgrHotswapAvailable()) return false;
  bool needs = false;
  int rc = ComgrHotswapNeedsTranspile(
      source_isa.c_str(), target_isa.c_str(), &needs);
  return (rc == 0) && needs;
}

RewriteResult TranspileCodeObject(void** elf_data, size_t* elf_size,
                                  const std::string& source_isa,
                                  const std::string& target_isa,
                                  TranspileStats* stats) {
  RewriteResult result = {HSA_STATUS_SUCCESS, 0, 0};

  if (!ComgrHotswapAvailable()) {
    std::cerr << "hotswap: COMGR not available for transpile\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  void* out_elf = nullptr;
  size_t out_elf_size = 0;
  ComgrHotswapResult comgr_result = {};

  int rc = ComgrHotswapRewrite(
      *elf_data, *elf_size,
      source_isa.c_str(), target_isa.c_str(),
      COMGR_HOTSWAP_FLAG_TRANSPILE, nullptr,
      &out_elf, &out_elf_size, &comgr_result);

  if (rc != 0) {
    std::cerr << "hotswap: COMGR transpile failed (rc=" << rc << ")\n";
    result.status = HSA_STATUS_ERROR;
    if (out_elf) std::free(out_elf);
    return result;
  }

  if (out_elf) {
    *elf_data = out_elf;
    *elf_size = out_elf_size;
  }

  result.rules_matched = comgr_result.rules_matched;
  result.trampolines_added = comgr_result.trampolines_added;

  if (stats) {
    stats->translated_passthrough = comgr_result.transpile_passthrough;
    stats->translated_renamed = comgr_result.transpile_renamed;
    stats->translated_waitcnt = comgr_result.transpile_waitcnt;
    stats->unsupported_skipped = comgr_result.transpile_unsupported;
  }

  return result;
}

} // namespace hotswap
} // namespace rocr

// ── C-linkage wrappers ───────────────────────────────────────────────────────

extern "C" __attribute__((visibility("default")))
int rocr_hotswap_retarget(void* elf_data, size_t elf_size,
                          const char* source_isa, const char* target_isa) {
  auto result = rocr::hotswap::RetargetCodeObject(
      elf_data, elf_size, std::string(source_isa), std::string(target_isa));
  return result.rules_matched;
}

extern "C" __attribute__((visibility("default")))
int rocr_hotswap_gfx1250_b0_to_a0(void* elf_data, size_t elf_size) {
  void* out = nullptr;
  size_t out_size = 0;
  auto result = rocr::hotswap::RetargetCodeObjectB0A0Grow(
      elf_data, elf_size, &out, &out_size);
  if (out && out != elf_data) {
    if (out_size <= elf_size)
      std::memcpy(elf_data, out, out_size);
    else
      result.rules_matched = 0;
    std::free(out);
  }
  return result.rules_matched;
}

extern "C" __attribute__((visibility("default")))
int rocr_hotswap_gfx1250_b0_to_a0_grow(
    const void* elf_data, size_t elf_size,
    void** out_data, size_t* out_size) {
  auto result = rocr::hotswap::RetargetCodeObjectB0A0Grow(
      elf_data, elf_size, out_data, out_size);
  return result.rules_matched;
}

extern "C" __attribute__((visibility("default")))
int rocr_hotswap_classify_wmma_nops(const char* mnemonic, int* b0, int* a0) {
  auto req = rocr::hotswap::ClassifyWmmaNops(std::string(mnemonic));
  if (b0) *b0 = req.b0_nops;
  if (a0) *a0 = req.a0_nops;
  return (req.a0_nops > req.b0_nops) ? 1 : 0;
}

extern "C" __attribute__((visibility("default")))
int rocr_hotswap_assemble_inst(const char* asm_str,
                                uint8_t* out_bytes, int max_bytes) {
  if (!asm_str || !out_bytes || max_bytes <= 0) return -1;

  if (!rocr::hotswap::ComgrHotswapAvailable()) return -1;

  // Build a minimal ELF with a .text section containing the instruction.
  // Use the COMGR rewrite with B0_TO_A0 flag on a dummy ELF to trigger
  // the assembly pipeline indirectly. For a clean approach, shell out to
  // llvm-mc if available.
  char tmpasm[] = "/tmp/hotswap_asm_XXXXXX";
  int fd = mkstemp(tmpasm);
  if (fd < 0) return -1;
  std::string full_asm = ".text\n" + std::string(asm_str) + "\n";
  write(fd, full_asm.c_str(), full_asm.size());
  close(fd);

  std::string tmpobj = std::string(tmpasm) + ".o";
  const char* llvm_mc = std::getenv("HSA_HOTSWAP_LLVM_MC");
  if (!llvm_mc || !*llvm_mc) llvm_mc = "llvm-mc";
  std::string cmd = std::string(llvm_mc) + " -triple=amdgcn-amd-amdhsa -mcpu=gfx1250"
      " -filetype=obj " + std::string(tmpasm) + " -o " + tmpobj + " 2>/dev/null";
  int rc = system(cmd.c_str());
  unlink(tmpasm);

  if (rc != 0) {
    unlink(tmpobj.c_str());
    return -1;
  }

  // Read the object file and extract .text
  std::ifstream ifs(tmpobj, std::ios::binary | std::ios::ate);
  if (!ifs) { unlink(tmpobj.c_str()); return -1; }
  auto sz = ifs.tellg();
  ifs.seekg(0);
  std::vector<uint8_t> obj(sz);
  ifs.read(reinterpret_cast<char*>(obj.data()), sz);
  ifs.close();
  unlink(tmpobj.c_str());

  rocr::hotswap::ElfInfo info;
  if (!rocr::hotswap::ParseElfInfo(obj.data(), obj.size(), info))
    return -1;
  if (info.text_size == 0 || info.text_size > static_cast<uint64_t>(max_bytes))
    return -1;

  std::memcpy(out_bytes, obj.data() + info.text_offset, info.text_size);
  return static_cast<int>(info.text_size);
}
