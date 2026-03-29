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

#ifndef ROCR_HOTSWAP_CORE_HPP
#define ROCR_HOTSWAP_CORE_HPP

#include "hotswap_shared_types.h"
#include "trampoline.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rocr {
namespace hotswap {

// ── ELF helpers ──────────────────────────────────────────────────────────────
// ElfSection, ElfSymbol, ElfInfo, and ExtractCPU are defined in
// hotswap_shared_types.h (shared with COMGR's comgr-hotswap-elf.inc).

bool ParseElfInfo(const uint8_t* elf, size_t elf_size, ElfInfo& info);

std::string FindKernelAtOffset(const ElfInfo& elf_info, uint64_t text_offset);

// ── Instruction-level helpers (LLVM-free) ────────────────────────────────────

bool ApplyByteReplace(const struct RewriteRule& rule,
                      uint64_t inst_offset, uint32_t inst_size,
                      uint8_t* text, uint64_t text_size);

void UpdateKernelDescriptor(uint8_t* elf_data, size_t elf_size,
                            const ElfInfo& elf_info,
                            const std::string& kernel_name,
                            int32_t extra_vgprs, int32_t extra_sgprs);

std::vector<std::string> ExpandDs2AddrAsm(
    const std::string& printed_asm,
    const std::string& from_mnemonic,
    const std::string& to_mnemonic);

bool ShouldDump();

// ── NOP sled management ─────────────────────────────────────────────────────

struct NopSled {
  uint64_t start;
  uint64_t end;
  uint64_t write_pos;
};

NopSled* FindNearestSled(std::vector<NopSled>& sleds,
                         uint64_t offset, uint64_t needed);

// ── ELF growth ──────────────────────────────────────────────────────────────

uint8_t* GrowElfWithTrampolines(
    const uint8_t* elf, size_t elf_size,
    const ElfInfo& elf_info,
    const std::vector<Trampoline>& trampolines,
    size_t* out_size);

// ── WMMA hazard classification ──────────────────────────────────────────────

struct WmmaNopReq {
  int b0_nops;
  int a0_nops;
};

struct WmmaHazard {
  size_t wmma_idx;
  size_t valu_idx;
  int existing_nops;
  int needed_nops;
  int deficit;
};

WmmaNopReq ClassifyWmmaNops(const std::string& mnemonic);
bool IsValuInst(const std::string& mnemonic);
bool RangesOverlap(int base1, int count1, int base2, int count2);
std::string FormatVgprRange(int base, int count);

// ── Mnemonic swap tables ────────────────────────────────────────────────────

extern const std::pair<std::string, std::string> kClusterLoadSwaps[];
extern const size_t kClusterLoadSwapsSize;

extern const std::pair<std::string, std::string> kDs2AddrSwaps[];
extern const size_t kDs2AddrSwapsSize;

} // namespace hotswap
} // namespace rocr

#endif // ROCR_HOTSWAP_CORE_HPP
