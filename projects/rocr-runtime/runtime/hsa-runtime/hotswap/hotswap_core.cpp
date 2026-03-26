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

// KEEP IN SYNC: ElfSection, ElfSymbol, ElfInfo, ParseElfInfo, ExtractCPU,
// and FindKernelAtOffset are duplicated in
// llvm-project/amd/comgr/src/comgr-hotswap-elf.h.

#include "hotswap_core.hpp"
#include "hotswap_rules.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace rocr {
namespace hotswap {

// ── ExtractCPU ───────────────────────────────────────────────────────────────

std::string ExtractCPU(const std::string& isa_name) {
  size_t pos = isa_name.rfind("gfx");
  if (pos != std::string::npos) {
    std::string cpu;
    for (size_t i = pos; i < isa_name.size(); ++i) {
      char c = isa_name[i];
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'Z'))
        cpu += c;
      else
        break;
    }
    return cpu;
  }
  return "";
}

// ── ELF parsing ──────────────────────────────────────────────────────────────

bool ParseElfInfo(const uint8_t* elf, size_t elf_size, ElfInfo& info) {
  if (elf_size < 64) return false;
  if (elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F')
    return false;
  if (elf[4] != 2) return false;

  uint64_t e_shoff;
  uint16_t e_shentsize, e_shnum, e_shstrndx;
  std::memcpy(&e_shoff, elf + 40, 8);
  std::memcpy(&e_shentsize, elf + 58, 2);
  std::memcpy(&e_shnum, elf + 60, 2);
  std::memcpy(&e_shstrndx, elf + 62, 2);

  if (e_shoff == 0 || e_shnum == 0) return false;
  if (e_shoff + static_cast<uint64_t>(e_shnum) * e_shentsize > elf_size)
    return false;

  const char* shstrtab = nullptr;
  uint64_t shstrtab_size = 0;
  if (e_shstrndx < e_shnum) {
    const uint8_t* sh = elf + e_shoff + e_shstrndx * e_shentsize;
    uint64_t sh_offset, sh_size;
    std::memcpy(&sh_offset, sh + 24, 8);
    std::memcpy(&sh_size, sh + 32, 8);
    if (sh_offset + sh_size <= elf_size) {
      shstrtab = reinterpret_cast<const char*>(elf + sh_offset);
      shstrtab_size = sh_size;
    }
  }

  info.sections.resize(e_shnum);
  for (uint16_t i = 0; i < e_shnum; ++i) {
    const uint8_t* sh = elf + e_shoff + i * e_shentsize;
    auto& sec = info.sections[i];
    std::memcpy(&sec.name_idx, sh, 4);
    std::memcpy(&sec.type, sh + 4, 4);
    std::memcpy(&sec.addr, sh + 16, 8);
    std::memcpy(&sec.offset, sh + 24, 8);
    std::memcpy(&sec.size, sh + 32, 8);

    if (shstrtab && sec.name_idx < shstrtab_size) {
      sec.name = shstrtab + sec.name_idx;
    }

    if (sec.name == ".text") {
      info.text_section_idx = i;
      info.text_offset = sec.offset;
      info.text_size = sec.size;
      info.text_addr = sec.addr;
    }
  }

  for (uint16_t i = 0; i < e_shnum; ++i) {
    auto& sec = info.sections[i];
    if (sec.type != 2 && sec.type != 11)
      continue;

    const uint8_t* sh = elf + e_shoff + i * e_shentsize;
    uint32_t sh_link;
    std::memcpy(&sh_link, sh + 40, 4);

    const char* symstrtab = nullptr;
    uint64_t symstrtab_size = 0;
    if (sh_link < e_shnum) {
      auto& link_sec = info.sections[sh_link];
      if (link_sec.offset + link_sec.size <= elf_size) {
        symstrtab = reinterpret_cast<const char*>(elf + link_sec.offset);
        symstrtab_size = link_sec.size;
      }
    }

    size_t sym_count = sec.size / 24;
    for (size_t j = 0; j < sym_count; ++j) {
      if (sec.offset + (j + 1) * 24 > elf_size) break;
      const uint8_t* sym_entry = elf + sec.offset + j * 24;

      ElfSymbol sym;
      uint32_t st_name;
      std::memcpy(&st_name, sym_entry, 4);
      sym.info = sym_entry[4];
      std::memcpy(&sym.shndx, sym_entry + 6, 2);
      std::memcpy(&sym.value, sym_entry + 8, 8);
      std::memcpy(&sym.size, sym_entry + 16, 8);

      if (symstrtab && st_name < symstrtab_size) {
        sym.name = symstrtab + st_name;
      }

      info.symbols.push_back(std::move(sym));
    }
  }

  return info.text_section_idx >= 0;
}

std::string FindKernelAtOffset(const ElfInfo& elf_info, uint64_t text_offset) {
  for (auto& sym : elf_info.symbols) {
    uint8_t sym_type = sym.info & 0xf;
    if (sym_type != 2 && sym_type != 10)
      continue;
    if (sym.shndx != static_cast<uint16_t>(elf_info.text_section_idx))
      continue;

    uint64_t sym_start = sym.value;
    uint64_t sym_end = sym.value + sym.size;
    if (text_offset >= sym_start && text_offset < sym_end) {
      return sym.name;
    }
  }
  return "";
}

// ── ApplyByteReplace ─────────────────────────────────────────────────────────

bool ApplyByteReplace(const RewriteRule& rule,
                      uint64_t inst_offset, uint32_t inst_size,
                      uint8_t* text, uint64_t text_size) {
  if (rule.replace_bytes.size() > inst_size) {
    std::cerr << "hotswap: replace_bytes larger than original instruction ("
              << rule.replace_bytes.size() << " > " << inst_size << ")\n";
    return false;
  }

  std::memcpy(text + inst_offset, rule.replace_bytes.data(),
              rule.replace_bytes.size());

  uint32_t remaining = inst_size - static_cast<uint32_t>(rule.replace_bytes.size());
  uint64_t pad_offset = inst_offset + rule.replace_bytes.size();
  while (remaining >= 4) {
    uint8_t nop[4];
    EncodeSNop(nop);
    std::memcpy(text + pad_offset, nop, 4);
    pad_offset += 4;
    remaining -= 4;
  }

  return true;
}

// ── UpdateKernelDescriptor ───────────────────────────────────────────────────

void UpdateKernelDescriptor(uint8_t* elf_data, size_t elf_size,
                            const ElfInfo& elf_info,
                            const std::string& kernel_name,
                            int32_t extra_vgprs, int32_t extra_sgprs) {
  std::string kd_name = kernel_name + ".kd";

  for (auto& sym : elf_info.symbols) {
    if (sym.name != kd_name) continue;
    if (sym.shndx >= elf_info.sections.size()) continue;

    auto& sec = elf_info.sections[sym.shndx];
    uint64_t kd_file_offset = sec.offset + sym.value;

    if (kd_file_offset + 64 > elf_size) continue;

    uint8_t* kd = elf_data + kd_file_offset;

    uint32_t rsrc1;
    std::memcpy(&rsrc1, kd + 48, 4);

    if (extra_vgprs > 0) {
      uint32_t current = rsrc1 & 0x3F;
      uint32_t extra_granules = (static_cast<uint32_t>(extra_vgprs) + 3) / 4;
      uint32_t new_val = current + extra_granules;
      if (new_val > 63) new_val = 63;
      rsrc1 = (rsrc1 & ~0x3Fu) | new_val;
    }

    if (extra_sgprs > 0) {
      uint32_t current = (rsrc1 >> 6) & 0xF;
      uint32_t extra_granules = (static_cast<uint32_t>(extra_sgprs) + 7) / 8;
      uint32_t new_val = current + extra_granules;
      if (new_val > 15) new_val = 15;
      rsrc1 = (rsrc1 & ~(0xFu << 6)) | (new_val << 6);
    }

    std::memcpy(kd + 48, &rsrc1, 4);
    return;
  }
}

// ── ShouldDump ───────────────────────────────────────────────────────────────

bool ShouldDump() {
  static int dump = -1;
  if (dump < 0) {
    const char* env = std::getenv("HSA_HOTSWAP_DUMP");
    dump = (env && env[0] == '1') ? 1 : 0;
  }
  return dump == 1;
}

// ── ExpandDs2AddrAsm ─────────────────────────────────────────────────────────

std::vector<std::string> ExpandDs2AddrAsm(
    const std::string& printed_asm,
    const std::string& from_mnemonic,
    const std::string& to_mnemonic) {
  size_t start = printed_asm.find_first_not_of(" \t");
  if (start == std::string::npos) return {};
  size_t mnem_end = printed_asm.find_first_of(" \t", start);
  if (mnem_end == std::string::npos) return {};

  std::string operand_str = printed_asm.substr(mnem_end);

  auto extractOffsetVal = [](const std::string& s, const std::string& key) -> std::string {
    size_t pos = s.find(key);
    if (pos == std::string::npos) return "0";
    size_t vstart = pos + key.size();
    size_t vend = vstart;
    while (vend < s.size() && s[vend] != ' ' && s[vend] != '\t' && s[vend] != ',')
      vend++;
    return s.substr(vstart, vend - vstart);
  };
  std::string off0_val = extractOffsetVal(operand_str, "offset0:");
  std::string off1_val = extractOffsetVal(operand_str, "offset1:");

  if (from_mnemonic.find("stride64") != std::string::npos) {
    uint32_t elem_bytes = (from_mnemonic.find("_b64") != std::string::npos) ? 8 : 4;
    uint32_t scale = 64 * elem_bytes;
    auto scaleVal = [scale](std::string& val) {
      if (val == "0" || val.empty()) return;
      try {
        uint32_t v = static_cast<uint32_t>(std::stoul(val, nullptr, 0));
        val = std::to_string(v * scale);
      } catch (...) {}
    };
    scaleVal(off0_val);
    scaleVal(off1_val);
  }

  auto removeToken = [](std::string& s, const std::string& prefix) {
    size_t pos = s.find(prefix);
    if (pos == std::string::npos) return;
    size_t end = pos + prefix.size();
    while (end < s.size() && s[end] != ' ' && s[end] != '\t' && s[end] != ',')
      end++;
    while (end < s.size() && (s[end] == ' ' || s[end] == '\t' || s[end] == ','))
      end++;
    s.erase(pos, end - pos);
  };
  removeToken(operand_str, "offset0:");
  removeToken(operand_str, "offset1:");

  std::vector<std::string> ops;
  {
    std::string rest = operand_str;
    size_t s = rest.find_first_not_of(" \t");
    if (s != std::string::npos) rest = rest.substr(s);
    while (!rest.empty()) {
      size_t comma = rest.find(',');
      if (comma == std::string::npos) {
        std::string tok = rest;
        s = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (s != std::string::npos && e != std::string::npos)
          tok = tok.substr(s, e - s + 1);
        if (!tok.empty()) ops.push_back(tok);
        break;
      }
      std::string tok = rest.substr(0, comma);
      s = tok.find_first_not_of(" \t");
      size_t e = tok.find_last_not_of(" \t");
      if (s != std::string::npos && e != std::string::npos)
        tok = tok.substr(s, e - s + 1);
      if (!tok.empty()) ops.push_back(tok);
      rest = rest.substr(comma + 1);
      s = rest.find_first_not_of(" \t");
      if (s != std::string::npos) rest = rest.substr(s);
    }
  }

  std::vector<std::string> clean_ops;
  for (auto& op : ops) {
    if (op.find("offset") == std::string::npos)
      clean_ops.push_back(op);
  }
  ops = clean_ops;

  auto extractFirstOfPair = [](const std::string& reg) -> std::string {
    size_t bracket = reg.find('[');
    if (bracket == std::string::npos) return reg;
    size_t colon = reg.find(':', bracket);
    if (colon == std::string::npos) return reg;
    return reg.substr(0, bracket) + reg.substr(bracket + 1, colon - bracket - 1);
  };
  auto extractSecondOfPair = [](const std::string& reg) -> std::string {
    size_t colon = reg.find(':');
    size_t close = reg.find(']');
    if (colon == std::string::npos || close == std::string::npos) return reg;
    return reg.substr(0, reg.find('[')) + reg.substr(colon + 1, close - colon - 1);
  };

  auto withOffset = [](const std::string& base, const std::string& off) -> std::string {
    if (off == "0" || off.empty()) return base;
    return base + " offset:" + off;
  };

  bool is_load = (from_mnemonic.find("ds_load") == 0);
  bool is_store = (from_mnemonic.find("ds_store_") == 0 &&
                   from_mnemonic.find("xchg") == std::string::npos);
  bool is_xchg = (from_mnemonic.find("ds_storexchg") == 0);

  if (is_load && ops.size() >= 2) {
    std::string d0 = extractFirstOfPair(ops[0]);
    std::string d1 = extractSecondOfPair(ops[0]);
    std::string addr = ops[1];
    return {
      withOffset(to_mnemonic + " " + d0 + ", " + addr, off0_val),
      withOffset(to_mnemonic + " " + d1 + ", " + addr, off1_val),
    };
  }

  if (is_store && ops.size() >= 3) {
    return {
      withOffset(to_mnemonic + " " + ops[0] + ", " + ops[1], off0_val),
      withOffset(to_mnemonic + " " + ops[0] + ", " + ops[2], off1_val),
    };
  }

  if (is_xchg && ops.size() >= 4) {
    std::string d0 = extractFirstOfPair(ops[0]);
    std::string d1 = extractSecondOfPair(ops[0]);
    return {
      withOffset(to_mnemonic + " " + d0 + ", " + ops[1] + ", " + ops[2], off0_val),
      withOffset(to_mnemonic + " " + d1 + ", " + ops[1] + ", " + ops[3], off1_val),
    };
  }

  return {};
}

// ── NOP sled management ─────────────────────────────────────────────────────

NopSled* FindNearestSled(std::vector<NopSled>& sleds,
                         uint64_t offset, uint64_t needed) {
  NopSled* best = nullptr;
  int64_t best_dist = INT64_MAX;
  for (auto& sled : sleds) {
    if (sled.write_pos + needed > sled.end) continue;
    int64_t dist = std::abs(static_cast<int64_t>(sled.write_pos) -
                            static_cast<int64_t>(offset));
    if (dist < 131072 && dist < best_dist) {
      best = &sled;
      best_dist = dist;
    }
  }
  return best;
}

// ── GrowElfWithTrampolines ──────────────────────────────────────────────────

uint8_t* GrowElfWithTrampolines(
    const uint8_t* elf, size_t elf_size,
    const ElfInfo& elf_info,
    const std::vector<Trampoline>& trampolines,
    size_t* out_size) {
  size_t tramp_total = 0;
  for (auto& t : trampolines) tramp_total += t.bytes.size();
  if (tramp_total == 0) return nullptr;

  size_t new_elf_size = elf_size + tramp_total;
  uint8_t* new_elf = static_cast<uint8_t*>(std::malloc(new_elf_size));
  if (!new_elf) return nullptr;

  uint64_t text_end = elf_info.text_offset + elf_info.text_size;
  std::memcpy(new_elf, elf, text_end);

  uint64_t tramp_pos = text_end;
  for (auto& t : trampolines) {
    std::memcpy(new_elf + tramp_pos, t.bytes.data(), t.bytes.size());
    tramp_pos += t.bytes.size();
  }

  if (text_end < elf_size) {
    std::memcpy(new_elf + tramp_pos, elf + text_end, elf_size - text_end);
  }

  uint64_t e_shoff;
  uint16_t e_shentsize;
  std::memcpy(&e_shoff, new_elf + 40, 8);
  std::memcpy(&e_shentsize, new_elf + 58, 2);

  if (e_shoff >= text_end) {
    uint64_t new_shoff = e_shoff + tramp_total;
    std::memcpy(new_elf + 40, &new_shoff, 8);
    e_shoff = new_shoff;
  }

  uint16_t e_shnum;
  std::memcpy(&e_shnum, new_elf + 60, 2);

  for (uint16_t i = 0; i < e_shnum; ++i) {
    uint8_t* sh = new_elf + e_shoff + i * e_shentsize;
    uint64_t sh_offset;
    std::memcpy(&sh_offset, sh + 24, 8);

    if (sh_offset == elf_info.text_offset) {
      uint64_t new_text_size = elf_info.text_size + tramp_total;
      std::memcpy(sh + 32, &new_text_size, 8);
    } else if (sh_offset > elf_info.text_offset) {
      uint64_t new_offset = sh_offset + tramp_total;
      std::memcpy(sh + 24, &new_offset, 8);
    }
  }

  uint64_t e_phoff;
  uint16_t e_phentsize, e_phnum;
  std::memcpy(&e_phoff, new_elf + 32, 8);
  std::memcpy(&e_phentsize, new_elf + 54, 2);
  std::memcpy(&e_phnum, new_elf + 56, 2);

  for (uint16_t i = 0; i < e_phnum; ++i) {
    uint8_t* ph = new_elf + e_phoff + i * e_phentsize;
    uint64_t p_offset, p_filesz, p_memsz;
    std::memcpy(&p_offset, ph + 8, 8);
    std::memcpy(&p_filesz, ph + 32, 8);
    std::memcpy(&p_memsz, ph + 40, 8);

    if (p_offset <= elf_info.text_offset &&
        p_offset + p_filesz >= text_end) {
      p_filesz += tramp_total;
      p_memsz += tramp_total;
      std::memcpy(ph + 32, &p_filesz, 8);
      std::memcpy(ph + 40, &p_memsz, 8);
    } else if (p_offset > elf_info.text_offset) {
      p_offset += tramp_total;
      std::memcpy(ph + 8, &p_offset, 8);
    }
  }

  *out_size = new_elf_size;
  return new_elf;
}

// ── WMMA helpers ─────────────────────────────────────────────────────────────

WmmaNopReq ClassifyWmmaNops(const std::string& mnemonic) {
  bool is_wmma = (mnemonic.find("v_wmma") == 0);
  bool is_swmmac = (mnemonic.find("v_swmmac") == 0);
  if (!is_wmma && !is_swmmac) return {4, 4};

  if (mnemonic.find("_iu8") != std::string::npos ||
      mnemonic.find("_iu4") != std::string::npos) {
    return {8, 4};
  }

  if (mnemonic.find("f8f6f4") != std::string::npos) {
    return {1, 4};
  }

  bool has_f8 = (mnemonic.find("_fp8") != std::string::npos ||
                 mnemonic.find("_f8") != std::string::npos ||
                 mnemonic.find("_bf8") != std::string::npos);
  if (has_f8) {
    if (mnemonic.find("16x16x128") != std::string::npos) {
      return {3, 4};
    }
    return {1, 4};
  }

  if (mnemonic.find("_f16") != std::string::npos ||
      mnemonic.find("_bf16") != std::string::npos) {
    return {4, 4};
  }

  return {4, 4};
}

bool IsValuInst(const std::string& mnemonic) {
  if (mnemonic.size() < 2) return false;
  if (mnemonic[0] != 'v' || mnemonic[1] != '_') return false;
  if (mnemonic == "v_nop") return false;
  if (mnemonic.find("v_wmma") == 0) return false;
  if (mnemonic.find("v_swmmac") == 0) return false;
  return true;
}

bool RangesOverlap(int base1, int count1, int base2, int count2) {
  if (base1 < 0 || base2 < 0) return false;
  int end1 = base1 + count1;
  int end2 = base2 + count2;
  return base1 < end2 && base2 < end1;
}

std::string FormatVgprRange(int base, int count) {
  if (count <= 1) return "v" + std::to_string(base);
  return "v[" + std::to_string(base) + ":" + std::to_string(base + count - 1) + "]";
}

// ── Mnemonic swap tables ────────────────────────────────────────────────────

const std::pair<std::string, std::string> kClusterLoadSwaps[] = {
  {"cluster_load_b32",               "global_load_b32"},
  {"cluster_load_b64",               "global_load_b64"},
  {"cluster_load_b128",              "global_load_b128"},
  {"cluster_load_async_to_lds_b8",   "global_load_async_to_lds_b8"},
  {"cluster_load_async_to_lds_b32",  "global_load_async_to_lds_b32"},
  {"cluster_load_async_to_lds_b64",  "global_load_async_to_lds_b64"},
  {"cluster_load_async_to_lds_b128", "global_load_async_to_lds_b128"},
};
const size_t kClusterLoadSwapsSize = sizeof(kClusterLoadSwaps) / sizeof(kClusterLoadSwaps[0]);

const std::pair<std::string, std::string> kDs2AddrSwaps[] = {
  {"ds_load_2addr_b32",                   "ds_load_b32"},
  {"ds_load_2addr_b64",                   "ds_load_b64"},
  {"ds_load_2addr_stride64_b32",          "ds_load_b32"},
  {"ds_load_2addr_stride64_b64",          "ds_load_b64"},
  {"ds_store_2addr_b32",                  "ds_store_b32"},
  {"ds_store_2addr_b64",                  "ds_store_b64"},
  {"ds_store_2addr_stride64_b32",         "ds_store_b32"},
  {"ds_store_2addr_stride64_b64",         "ds_store_b64"},
  {"ds_storexchg_2addr_rtn_b32",          "ds_storexchg_rtn_b32"},
  {"ds_storexchg_2addr_rtn_b64",          "ds_storexchg_rtn_b64"},
  {"ds_storexchg_2addr_stride64_rtn_b32", "ds_storexchg_rtn_b32"},
  {"ds_storexchg_2addr_stride64_rtn_b64", "ds_storexchg_rtn_b64"},
};
const size_t kDs2AddrSwapsSize = sizeof(kDs2AddrSwaps) / sizeof(kDs2AddrSwaps[0]);

} // namespace hotswap
} // namespace rocr
