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

#include "trampoline.hpp"

#include <cstring>
#include <iostream>

namespace rocr {
namespace hotswap {

static constexpr uint32_t S_BRANCH_GFX9   = 0xBF820000u;
static constexpr uint32_t S_BRANCH_GFX12  = 0xBFA00000u;
static constexpr uint32_t S_NOP_OPCODE    = 0xBF800000u;

bool EncodeSBranch(uint64_t from_offset, uint64_t to_offset,
                   uint8_t out_bytes[4], bool gfx12) {
  int64_t byte_delta = static_cast<int64_t>(to_offset) -
                       static_cast<int64_t>(from_offset) - 4;

  if (byte_delta % 4 != 0) {
    std::cerr << "hotswap: branch offset not dword-aligned\n";
    return false;
  }

  int64_t dword_offset = byte_delta / 4;

  if (dword_offset < -32768 || dword_offset > 32767) {
    std::cerr << "hotswap: branch offset out of range (" << dword_offset
              << " dwords)\n";
    return false;
  }

  uint32_t opcode = gfx12 ? S_BRANCH_GFX12 : S_BRANCH_GFX9;
  uint32_t encoded = opcode | (static_cast<uint16_t>(dword_offset) & 0xFFFF);
  std::memcpy(out_bytes, &encoded, 4);
  return true;
}

void EncodeSNop(uint8_t out_bytes[4]) {
  uint32_t encoded = S_NOP_OPCODE;
  std::memcpy(out_bytes, &encoded, 4);
}

} // namespace hotswap
} // namespace rocr
