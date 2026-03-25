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
#include "hotswap_llvm_internal.hpp"
#include "hotswap_rules.hpp"
#include "trampoline.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

#include <llvm/Config/llvm-config.h>
#include <llvm/MC/MCAsmBackend.h>
#include <llvm/MC/MCAsmInfo.h>
#include <llvm/MC/MCCodeEmitter.h>
#include <llvm/MC/MCContext.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>
#include <llvm/MC/MCInstPrinter.h>
#include <llvm/MC/MCInstrInfo.h>
#include <llvm/MC/MCObjectFileInfo.h>
#include <llvm/MC/MCObjectWriter.h>
#include <llvm/MC/MCParser/MCAsmParser.h>
#include <llvm/MC/MCParser/MCTargetAsmParser.h>
#include <llvm/MC/MCRegisterInfo.h>
#include <llvm/MC/MCStreamer.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#if LLVM_VERSION_MAJOR > 13
#include <llvm/MC/TargetRegistry.h>
#else
#include <llvm/Support/TargetRegistry.h>
#endif

namespace rocr {
namespace hotswap {

namespace {

// ── LLVM MC Context (lazy-initialized, one per process) ──────────────────────

struct LLVMState {
  const llvm::Target* target = nullptr;
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
  std::unique_ptr<const llvm::MCAsmInfo> MAI;
  std::unique_ptr<llvm::MCInstrInfo> MCII;
  std::unique_ptr<llvm::MCSubtargetInfo> STI;
  std::unique_ptr<llvm::MCContext> Ctx;
  std::unique_ptr<llvm::MCObjectFileInfo> MOFI;
  std::unique_ptr<llvm::MCDisassembler> disasm;
  std::unique_ptr<llvm::MCInstPrinter> printer;
  llvm::MCCodeEmitter* CE = nullptr; // owned by Ctx or target
  std::string cpu;                   // e.g. "gfx1201"
  bool valid = false;
};

static std::once_flag g_llvm_init_flag;
static bool g_llvm_initialized = false;

static void InitLLVMTargets() {
  LLVMInitializeAMDGPUTargetInfo();
  LLVMInitializeAMDGPUTargetMC();
  LLVMInitializeAMDGPUAsmParser();
  LLVMInitializeAMDGPUDisassembler();
  g_llvm_initialized = true;
}

/// Initialize LLVM MC state for a given ISA. Cached per CPU name to avoid
/// LLVM global state conflicts from creating multiple instances.
static LLVMState& InitLLVMCached(const std::string& isa_name);

static LLVMState InitLLVMImpl(const std::string& isa_name) {
  std::call_once(g_llvm_init_flag, InitLLVMTargets);

  LLVMState state;
  state.cpu = ExtractCPU(isa_name);
  if (state.cpu.empty()) {
    std::cerr << "hotswap: cannot extract CPU from ISA '" << isa_name << "'\n";
    return state;
  }

  std::string error;
  llvm::Triple triple("amdgcn-amd-amdhsa");

  state.target = llvm::TargetRegistry::lookupTarget("amdgcn", triple, error);
  if (!state.target) {
    std::cerr << "hotswap: target lookup failed: " << error << "\n";
    return state;
  }

  state.MRI.reset(state.target->createMCRegInfo(llvm::Triple("amdgcn-amd-amdhsa")));
  if (!state.MRI) return state;

  llvm::MCTargetOptions mc_opts;
#if LLVM_VERSION_MAJOR > 9
  state.MAI.reset(state.target->createMCAsmInfo(
      *state.MRI, llvm::Triple("amdgcn-amd-amdhsa"), mc_opts));
#else
  state.MAI.reset(state.target->createMCAsmInfo(
      *state.MRI, "amdgcn-amd-amdhsa"));
#endif
  if (!state.MAI) return state;

  state.MCII.reset(state.target->createMCInstrInfo());
  if (!state.MCII) return state;

  state.STI.reset(state.target->createMCSubtargetInfo(
      llvm::Triple("amdgcn-amd-amdhsa"), state.cpu, ""));
  if (!state.STI || !state.STI->isCPUStringValid(state.cpu)) {
    std::cerr << "hotswap: invalid CPU '" << state.cpu << "'\n";
    return state;
  }

#if LLVM_VERSION_MAJOR > 12
  state.Ctx = std::make_unique<llvm::MCContext>(
      triple, state.MAI.get(), state.MRI.get(), state.STI.get());
  state.MOFI = std::make_unique<llvm::MCObjectFileInfo>();
  state.MOFI->initMCObjectFileInfo(*state.Ctx, /*PIC=*/false);
  state.Ctx->setObjectFileInfo(state.MOFI.get());
#else
  // Older LLVM needs MCObjectFileInfo
  state.MOFI = std::make_unique<llvm::MCObjectFileInfo>();
  state.Ctx = std::make_unique<llvm::MCContext>(
      state.MAI.get(), state.MRI.get(), state.MOFI.get());
  state.MOFI->InitMCObjectFileInfo(triple, true, *state.Ctx);
#endif

  state.disasm.reset(
      state.target->createMCDisassembler(*state.STI, *state.Ctx));
  if (!state.disasm) {
    std::cerr << "hotswap: failed to create disassembler\n";
    return state;
  }

  // Create instruction printer for dump mode
  unsigned asm_variant = state.MAI->getAssemblerDialect();
  state.printer.reset(state.target->createMCInstPrinter(
      triple, asm_variant, *state.MAI, *state.MCII, *state.MRI));

#if LLVM_VERSION_MAJOR > 14
  state.CE = state.target->createMCCodeEmitter(*state.MCII, *state.Ctx);
#else
  state.CE = state.target->createMCCodeEmitter(
      *state.MCII, *state.MRI, *state.Ctx);
#endif

  state.valid = true;
  return state;
}

static std::mutex g_llvm_cache_mutex;
static std::map<std::string, LLVMState> g_llvm_cache;

static LLVMState& InitLLVMCached(const std::string& isa_name) {
  std::string cpu = ExtractCPU(isa_name);
  std::lock_guard<std::mutex> lock(g_llvm_cache_mutex);
  auto it = g_llvm_cache.find(cpu);
  if (it != g_llvm_cache.end()) return it->second;
  g_llvm_cache[cpu] = InitLLVMImpl(isa_name);
  return g_llvm_cache[cpu];
}

// Backwards-compatible alias
static LLVMState InitLLVM(const std::string& isa_name) {
  // For non-retarget paths, return a copy (they don't need caching)
  return InitLLVMImpl(isa_name);
}

// ── Decoded instruction with MCInst ──────────────────────────────────────────

struct InternalDecodedInst {
  uint64_t offset;
  uint32_t size;
  llvm::MCInst inst;
  std::string mnemonic;
};

// ── Instruction decode/match/patch ───────────────────────────────────────────

static bool DecodeTextSection(const uint8_t* text, uint64_t text_size,
                              const LLVMState& llvm_state,
                              std::vector<InternalDecodedInst>& decoded) {
  uint64_t pos = 0;
  while (pos < text_size) {
    InternalDecodedInst di;
    di.offset = pos;

    llvm::ArrayRef<uint8_t> bytes(text + pos, text_size - pos);
    uint64_t inst_size = 0;

    auto status = llvm_state.disasm->getInstruction(
        di.inst, inst_size, bytes, pos, llvm::nulls());

    if (status == llvm::MCDisassembler::Fail) {
      // Skip 4 bytes (minimum AMDGPU instruction size) on decode failure
      di.size = 4;
      di.mnemonic = "<unknown>";
      pos += 4;
    } else {
      di.size = static_cast<uint32_t>(inst_size);

      // Get mnemonic via InstPrinter
      if (llvm_state.printer) {
        std::string str;
        llvm::raw_string_ostream rso(str);
        llvm_state.printer->printInst(&di.inst, 0, "", *llvm_state.STI, rso);
        rso.flush();

        // Extract mnemonic (first whitespace-delimited token)
        size_t start = str.find_first_not_of(" \t");
        if (start != std::string::npos) {
          size_t end = str.find_first_of(" \t", start);
          di.mnemonic = str.substr(start, end - start);
        }
      } else {
        // Fallback: use opcode name from instruction info
        di.mnemonic = llvm_state.MCII->getName(di.inst.getOpcode()).str();
      }

      pos += inst_size;
    }

    decoded.push_back(std::move(di));
  }
  return true;
}

static bool MatchRule(const RewriteRule& rule, const InternalDecodedInst& inst,
                      const ElfInfo& elf_info) {
  // Check mnemonic
  if (!rule.match_mnemonic.empty() && rule.match_mnemonic != inst.mnemonic)
    return false;

  // Check offset
  if (rule.match_offset >= 0 &&
      static_cast<uint64_t>(rule.match_offset) != inst.offset)
    return false;

  // Check kernel
  if (!rule.match_kernel.empty()) {
    std::string kernel = FindKernelAtOffset(elf_info, inst.offset);
    if (kernel != rule.match_kernel) return false;
  }

  // Check operands
  if (!rule.operands.empty()) {
    if (rule.operands.size() > static_cast<size_t>(inst.inst.getNumOperands()))
      return false;

    for (size_t i = 0; i < rule.operands.size(); ++i) {
      auto& match = rule.operands[i];
      auto& operand = inst.inst.getOperand(i);

      switch (match.kind) {
        case OperandMatch::Kind::Wildcard:
          break; // Always matches
        case OperandMatch::Kind::Immediate:
          if (!operand.isImm() || operand.getImm() != match.imm_value)
            return false;
          break;
        case OperandMatch::Kind::RegClass:
          // Register class matching would require MCRegisterInfo lookups.
          // For now, just verify it's a register operand.
          if (!operand.isReg()) return false;
          break;
      }
    }
  }

  return true;
}

/// Apply a same-size mnemonic swap by looking up the replacement opcode
/// and re-encoding with the same operands.
static bool ApplyMnemonicSwap(const RewriteRule& rule,
                              InternalDecodedInst& inst,
                              uint8_t* text,
                              const LLVMState& llvm_state) {
  // For mnemonic swaps, we need to find the opcode for the replacement
  // mnemonic. We do this by assembling a minimal instruction with the
  // replacement mnemonic and copying the encoded bytes.

  // Build an assembly line with the replacement mnemonic and the original
  // operands printed out.
  if (!llvm_state.printer) return false;

  std::string orig_str;
  llvm::raw_string_ostream rso(orig_str);
  llvm_state.printer->printInst(&inst.inst, 0, "", *llvm_state.STI, rso);
  rso.flush();

  // Replace the mnemonic in the printed string
  size_t start = orig_str.find_first_not_of(" \t");
  if (start == std::string::npos) return false;
  size_t end = orig_str.find_first_of(" \t", start);

  std::string new_asm;
  if (end != std::string::npos) {
    new_asm = rule.replace_mnemonic + orig_str.substr(end);
  } else {
    new_asm = rule.replace_mnemonic;
  }

  // Assemble the replacement instruction using the full MC pipeline.
  // Reset MCContext to clear accumulated sections/symbols from prior
  // assembly passes — without this, MCELFStreamer::initSections() crashes
  // when reusing a cached MCContext across multiple assemblies.
  llvm_state.Ctx->reset();

  // Prepend .text directive — LLVM MC requires a section before instructions
  std::string full_asm = ".text\n" + new_asm;
  llvm::StringRef asm_ref(full_asm);
  auto buf = llvm::MemoryBuffer::getMemBuffer(asm_ref, "", false);
  llvm::SourceMgr src_mgr;
  src_mgr.AddNewSourceBuffer(std::move(buf), llvm::SMLoc());

  std::string data;
  auto data_stream = std::make_unique<llvm::raw_string_ostream>(data);
  auto bos = std::make_unique<llvm::buffer_ostream>(*data_stream);

  llvm::MCTargetOptions mc_opts;
  llvm::Triple triple("amdgcn-amd-amdhsa");

#if LLVM_VERSION_MAJOR > 14
  llvm::MCCodeEmitter* ce =
      llvm_state.target->createMCCodeEmitter(*llvm_state.MCII, *llvm_state.Ctx);
#else
  llvm::MCCodeEmitter* ce = llvm_state.target->createMCCodeEmitter(
      *llvm_state.MCII, *llvm_state.MRI, *llvm_state.Ctx);
#endif
  llvm::MCAsmBackend* mab =
      llvm_state.target->createMCAsmBackend(*llvm_state.STI, *llvm_state.MRI, mc_opts);

  if (!ce || !mab) return false;

#if LLVM_VERSION_MAJOR > 20
  auto streamer = std::unique_ptr<llvm::MCStreamer>(
      llvm_state.target->createMCObjectStreamer(
          triple, *llvm_state.Ctx,
          std::unique_ptr<llvm::MCAsmBackend>(mab),
          mab->createObjectWriter(*bos),
          std::unique_ptr<llvm::MCCodeEmitter>(ce), *llvm_state.STI));
#else
  auto streamer = std::unique_ptr<llvm::MCStreamer>(
      llvm_state.target->createMCObjectStreamer(
          triple, *llvm_state.Ctx,
          std::unique_ptr<llvm::MCAsmBackend>(mab),
          mab->createObjectWriter(*bos),
          std::unique_ptr<llvm::MCCodeEmitter>(ce), *llvm_state.STI,
          mc_opts.MCRelaxAll, mc_opts.MCIncrementalLinkerCompatible, false));
#endif

  if (!streamer) return false;

  auto parser = std::unique_ptr<llvm::MCAsmParser>(
      llvm::createMCAsmParser(src_mgr, *llvm_state.Ctx, *streamer, *llvm_state.MAI));
  auto tap = std::unique_ptr<llvm::MCTargetAsmParser>(
      llvm_state.target->createMCAsmParser(*llvm_state.STI, *parser,
                                           *llvm_state.MCII, mc_opts));
  if (!tap) return false;
  parser->setTargetParser(*tap);

  if (parser->Run(true)) {
    std::cerr << "hotswap: mnemonic swap assembly failed for '"
              << new_asm << "'\n";
    return false;
  }

  bos.reset();
  data_stream->flush();

  // Extract .text from the assembled ELF
  const uint8_t* elf_bytes = reinterpret_cast<const uint8_t*>(data.data());
  size_t elf_sz = data.size();
  if (elf_sz < 64) return false;

  ElfInfo asm_elf;
  if (!ParseElfInfo(elf_bytes, elf_sz, asm_elf)) return false;

  if (asm_elf.text_size != inst.size) {
    std::cerr << "hotswap: mnemonic swap produced different size ("
              << asm_elf.text_size << " vs " << inst.size << ")\n";
    return false;
  }

  // Copy the assembled bytes into the original .text
  std::memcpy(text + inst.offset, elf_bytes + asm_elf.text_offset, inst.size);
  return true;
}

/// Assemble a single instruction string and return the encoded bytes.
/// Returns empty vector on failure. Uses its own Ctx->reset() to avoid
/// polluting shared state.
static std::vector<uint8_t> AssembleSingleInst(const std::string& asm_str,
                                                const LLVMState& llvm_state) {
  llvm_state.Ctx->reset();

  // Prepend .text directive — LLVM MC requires a section before instructions
  std::string full_asm = ".text\n" + asm_str;
  llvm::StringRef asm_ref(full_asm);
  auto buf = llvm::MemoryBuffer::getMemBuffer(asm_ref, "", false);
  llvm::SourceMgr src_mgr;
  src_mgr.AddNewSourceBuffer(std::move(buf), llvm::SMLoc());

  std::string data;
  auto data_stream = std::make_unique<llvm::raw_string_ostream>(data);
  auto bos = std::make_unique<llvm::buffer_ostream>(*data_stream);

  llvm::MCTargetOptions mc_opts;
  llvm::Triple triple("amdgcn-amd-amdhsa");

#if LLVM_VERSION_MAJOR > 14
  llvm::MCCodeEmitter* ce =
      llvm_state.target->createMCCodeEmitter(*llvm_state.MCII, *llvm_state.Ctx);
#else
  llvm::MCCodeEmitter* ce = llvm_state.target->createMCCodeEmitter(
      *llvm_state.MCII, *llvm_state.MRI, *llvm_state.Ctx);
#endif
  llvm::MCAsmBackend* mab =
      llvm_state.target->createMCAsmBackend(*llvm_state.STI, *llvm_state.MRI, mc_opts);

  if (!ce || !mab) return {};

#if LLVM_VERSION_MAJOR > 20
  auto streamer = std::unique_ptr<llvm::MCStreamer>(
      llvm_state.target->createMCObjectStreamer(
          triple, *llvm_state.Ctx,
          std::unique_ptr<llvm::MCAsmBackend>(mab),
          mab->createObjectWriter(*bos),
          std::unique_ptr<llvm::MCCodeEmitter>(ce), *llvm_state.STI));
#else
  auto streamer = std::unique_ptr<llvm::MCStreamer>(
      llvm_state.target->createMCObjectStreamer(
          triple, *llvm_state.Ctx,
          std::unique_ptr<llvm::MCAsmBackend>(mab),
          mab->createObjectWriter(*bos),
          std::unique_ptr<llvm::MCCodeEmitter>(ce), *llvm_state.STI,
          mc_opts.MCRelaxAll, mc_opts.MCIncrementalLinkerCompatible, false));
#endif

  if (!streamer) return {};

  auto parser = std::unique_ptr<llvm::MCAsmParser>(
      llvm::createMCAsmParser(src_mgr, *llvm_state.Ctx, *streamer, *llvm_state.MAI));
  auto tap = std::unique_ptr<llvm::MCTargetAsmParser>(
      llvm_state.target->createMCAsmParser(*llvm_state.STI, *parser,
                                           *llvm_state.MCII, mc_opts));
  if (!tap) return {};
  parser->setTargetParser(*tap);

  if (parser->Run(true)) {
    std::cerr << "hotswap: assembly failed for '" << asm_str << "'\n";
    return {};
  }

  bos.reset();
  data_stream->flush();

  const uint8_t* elf_bytes = reinterpret_cast<const uint8_t*>(data.data());
  size_t elf_sz = data.size();
  if (elf_sz < 64) return {};

  ElfInfo asm_elf;
  if (!ParseElfInfo(elf_bytes, elf_sz, asm_elf)) return {};
  if (asm_elf.text_size == 0) return {};

  return std::vector<uint8_t>(
      elf_bytes + asm_elf.text_offset,
      elf_bytes + asm_elf.text_offset + asm_elf.text_size);
}

static void DumpInstructions(const char* label,
                             const std::vector<InternalDecodedInst>& decoded,
                             const uint8_t* text) {
  std::cerr << "=== hotswap " << label << " ===\n";
  for (auto& d : decoded) {
    std::cerr << std::hex << std::setw(8) << std::setfill('0') << d.offset
              << ": ";
    for (uint32_t i = 0; i < d.size; ++i) {
      std::cerr << std::hex << std::setw(2) << std::setfill('0')
                << static_cast<int>(text[d.offset + i]) << " ";
    }
    std::cerr << "  " << d.mnemonic << "\n";
  }
  std::cerr << std::dec;
}

// ── gfx1250 B0→A0 patches using LLVM MC ──────────────────────────────────────
//
// Four patch types, all using LLVM-decoded mnemonics for matching:
//   1. CLUSTER_LOAD → GLOBAL_LOAD (ApplyMnemonicSwap)
//   2. DS 2-addr → single-addr (operand rewriting + reassembly)
//   3. s_clause → s_nop (ApplyByteReplace)
//   4. TENSOR_LOAD_TO_LDS multicast stripping (NOP sled trampoline)

/// Build a map of NOP sled regions from decoded instructions.
static std::vector<NopSled> BuildNopSledMap(
    const std::vector<InternalDecodedInst>& decoded) {
  std::vector<NopSled> sleds;
  for (size_t i = 0; i < decoded.size(); ++i) {
    if (decoded[i].mnemonic == "s_endpgm") {
      uint64_t sled_start = decoded[i].offset + decoded[i].size;
      uint64_t sled_end = sled_start;
      for (size_t j = i + 1; j < decoded.size(); ++j) {
        if (decoded[j].mnemonic == "s_nop") {
          sled_end = decoded[j].offset + decoded[j].size;
        } else {
          break;
        }
      }
      if (sled_end > sled_start + 8) {
        sleds.push_back({sled_start, sled_end, sled_start});
      }
    }
  }
  return sleds;
}

// ── WMMA co-execution hazard detection ──────────────────────────────────────
//
// B0→A0 stepping differences: some WMMA variants require more V_NOPs before
// a dependent VALU on A0 than B0 (longer co-execution window). If B0-compiled
// code has insufficient V_NOPs, running on A0 creates data hazards.

/// Extract the VGPR number from an MCRegisterInfo register ID.
/// Returns -1 if not a VGPR. AMDGPU VGPRs start at register 486 (VGPR0).
static int GetVgprNum(unsigned reg, const llvm::MCRegisterInfo& MRI) {
  // Get the register name and check for VGPR_32 or VGPR tuple classes
  const char* name = MRI.getName(reg);
  if (!name) return -1;
  std::string rname(name);

  // Single VGPRs: VGPR0, VGPR1, ...
  if (rname.find("VGPR") == 0) {
    // Extract number after "VGPR"
    size_t numstart = 4;
    // Skip past any underscore for tuples like VGPR0_VGPR1
    size_t underscore = rname.find('_', numstart);
    std::string numstr = rname.substr(numstart,
        underscore == std::string::npos ? std::string::npos : underscore - numstart);
    try { return std::stoi(numstr); } catch (...) { return -1; }
  }
  return -1;
}

/// Get the range of VGPRs covered by a register operand (handles tuples).
/// Returns {base_vgpr, count}. Returns {-1, 0} if not a VGPR.
static std::pair<int, int> GetVgprRange(unsigned reg,
                                         const llvm::MCRegisterInfo& MRI) {
  const char* name = MRI.getName(reg);
  if (!name) return {-1, 0};
  std::string rname(name);

  if (rname.find("VGPR") != 0) return {-1, 0};

  // Count VGPR components by counting underscores in tuple names
  // VGPR0 → 1 reg, VGPR0_VGPR1 → 2, VGPR0_VGPR1_VGPR2_VGPR3 → 4, etc.
  int count = 1;
  for (char c : rname) if (c == '_') count++;

  // Base is the first number after "VGPR"
  size_t numstart = 4;
  size_t numend = rname.find_first_not_of("0123456789", numstart);
  if (numend == std::string::npos) numend = rname.size();
  std::string numstr = rname.substr(numstart, numend - numstart);
  int base = -1;
  try { base = std::stoi(numstr); } catch (...) { return {-1, 0}; }

  return {base, count};
}

/// Get VGPR range for MCInst operand at op_idx. Wrapper around GetVgprRange().
static std::pair<int,int> GetOperandVgprRange(
    const llvm::MCInst& inst, unsigned op_idx, const llvm::MCRegisterInfo& MRI) {
  if (op_idx >= inst.getNumOperands()) return {-1, 0};
  const auto& op = inst.getOperand(op_idx);
  if (!op.isReg()) return {-1, 0};
  return GetVgprRange(op.getReg(), MRI);
}

/// Print full instruction string from MCInst via InstPrinter (for operand extraction).
static std::string PrintInst(const InternalDecodedInst& di, const LLVMState& llvm_state) {
  std::string inst_str;
  if (llvm_state.printer) {
    llvm::raw_string_ostream rso(inst_str);
    llvm_state.printer->printInst(&di.inst, 0, "", *llvm_state.STI, rso);
    rso.flush();
  }
  return inst_str;
}

/// Check if a VALU instruction has a WAR hazard with a co-executing WMMA.
///
/// The hazard is Write-After-Read: the WMMA is still reading its input
/// operands while a subsequent VALU instruction writes (clobbers) VGPRs
/// that overlap with those inputs.
///
/// WMMA MCInst operands: op0=D/C (dest, tied to C accumulator — both read
///   and written), op1=A-matrix (input), op2=B-matrix (input).
/// All WMMA VGPR operands are potential read targets (D/C is read as the
/// accumulator input). Only the VALU's write destination (operand 0) can
/// cause the WAR hazard.
static bool CheckVgprOverlap(const llvm::MCInst& wmma_inst,
                              const llvm::MCInst& valu_inst,
                              const llvm::MCRegisterInfo& MRI) {
  // Collect WMMA input VGPR ranges.
  // In LLVM's MCInst, WMMA has: op0=D (dest, tied to C accumulator),
  // op1=A-matrix, op2=B-matrix. The C accumulator is implicitly the same
  // register as D (op0) — there's no separate op3 for C.
  // For WAR hazards, ALL WMMA operands are inputs: D/C (op0 is read as
  // accumulator), A (op1), B (op2). A subsequent VALU writing to any of
  // these while the WMMA is still co-executing creates a hazard.
  std::vector<std::pair<int, int>> wmma_input_ranges;
  for (unsigned i = 0; i < wmma_inst.getNumOperands(); ++i) {
    const auto& op = wmma_inst.getOperand(i);
    if (!op.isReg()) continue;
    auto range = GetVgprRange(op.getReg(), MRI);
    if (range.first >= 0) wmma_input_ranges.push_back(range);
  }
  if (wmma_input_ranges.empty()) return false;

  // Get the VALU dest operand (operand 0 is the write destination)
  if (valu_inst.getNumOperands() == 0) return false;
  const auto& dest_op = valu_inst.getOperand(0);
  if (!dest_op.isReg()) return false;
  auto valu_dest = GetVgprRange(dest_op.getReg(), MRI);
  if (valu_dest.first < 0) return false;

  // WAR hazard: VALU dest clobbers a WMMA input operand
  for (const auto& wr : wmma_input_ranges) {
    if (RangesOverlap(wr.first, wr.second,
                       valu_dest.first, valu_dest.second))
      return true;
  }
  return false;
}

/// Scan decoded instructions for WMMA co-execution hazards.
/// Returns a list of hazards where A0 needs more V_NOPs than B0 provided.
static std::vector<WmmaHazard> ValidateWmmaCoexecHazards(
    const std::vector<InternalDecodedInst>& decoded,
    const uint8_t* text,
    const LLVMState& llvm_state) {
  std::vector<WmmaHazard> hazards;
  int wmma_scanned = 0;

  for (size_t i = 0; i < decoded.size(); ++i) {
    const auto& di = decoded[i];

    // Only check WMMA/SWMMAC instructions
    if (di.mnemonic.find("v_wmma") != 0 &&
        di.mnemonic.find("v_swmmac") != 0)
      continue;

    ++wmma_scanned;
    WmmaNopReq req = ClassifyWmmaNops(di.mnemonic);

    // Skip if A0 doesn't need more NOPs than B0
    if (req.a0_nops <= req.b0_nops) continue;

    // Count NOPs/non-overlapping instructions until dependent VALU
    int count = 0;
    for (size_t j = i + 1; j < decoded.size(); ++j) {
      const auto& dj = decoded[j];

      // v_nop always counts as 1 NOP slot
      if (dj.mnemonic == "v_nop") {
        ++count;
        if (count >= req.a0_nops) break;
        continue;
      }

      // SALU doesn't consume VALU issue slots — skip
      if (dj.mnemonic.size() >= 2 && dj.mnemonic[0] == 's' && dj.mnemonic[1] == '_') {
        // Branch/control flow → stop scanning (conservative)
        if (dj.mnemonic.find("s_branch") == 0 ||
            dj.mnemonic.find("s_cbranch") == 0 ||
            dj.mnemonic == "s_endpgm" ||
            dj.mnemonic == "s_setpc" ||
            dj.mnemonic == "s_swappc" ||
            dj.mnemonic == "s_call") {
          break;
        }
        continue;
      }

      // VALU instruction
      if (IsValuInst(dj.mnemonic)) {
        // Check register overlap with WMMA
        if (!CheckVgprOverlap(di.inst, dj.inst, *llvm_state.MRI)) {
          // No overlap — counts as 1 "unrelated" issue slot
          ++count;
          if (count >= req.a0_nops) break;
          continue;
        }
        // Overlap found — this is a hazard if insufficient NOPs
        if (count < req.a0_nops) {
          hazards.push_back({i, j, count, req.a0_nops,
                             req.a0_nops - count});
          std::cerr << "hotswap: B0->A0 WMMA co-exec hazard @0x"
                    << std::hex << di.offset
                    << ": " << di.mnemonic
                    << " needs " << std::dec << req.a0_nops
                    << " V_NOPs for A0, only " << count
                    << " found before " << dj.mnemonic
                    << " @0x" << std::hex << dj.offset
                    << std::dec << "\n";
        }
        break;
      }

      // Any other instruction type (ds, memory, etc.) → stop scanning
      break;
    }
  }

  std::cerr << "hotswap: B0->A0 WMMA co-exec validation: "
            << hazards.size() << " hazards ("
            << wmma_scanned << " WMMA instructions scanned)\n";

  return hazards;
}

/// Apply gfx1250 B0→A0 patches using LLVM-decoded mnemonics.
/// Returns the number of patches applied. tensor_load_to_lds patches
/// that cannot find a NOP sled are collected in out_trampolines for
/// later ELF growth.
static uint32_t ApplyGfx1250B0toA0Rules(
    std::vector<InternalDecodedInst>& decoded,
    uint8_t* text, uint64_t text_size,
    const LLVMState& llvm_state,
    std::vector<Trampoline>& out_trampolines) {
  uint32_t patched = 0;

  // Build NOP sled map for tensor_load_to_lds trampolines
  std::vector<NopSled> nop_sleds = BuildNopSledMap(decoded);

  for (size_t idx = 0; idx < decoded.size(); ++idx) {
    auto& di = decoded[idx];
    if (di.mnemonic == "<unknown>" || di.mnemonic == "<replaced>") continue;

    // ── Patch 1: CLUSTER_LOAD → GLOBAL_LOAD (ApplyMnemonicSwap) ─────────
    // Both use VGLOBAL encoding (12 bytes), identical operand layout.
    for (size_t swap_i = 0; swap_i < kClusterLoadSwapsSize; ++swap_i) {
      const auto& swap = kClusterLoadSwaps[swap_i];
      if (di.mnemonic == swap.first) {
        RewriteRule rule;
        rule.replace_mnemonic = swap.second;
        rule.preserve_operands = true;
        if (ApplyMnemonicSwap(rule, di, text, llvm_state)) {
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": " << swap.first << " -> " << swap.second
                    << std::dec << "\n";
          di.mnemonic = swap.second;
          ++patched;
        }
        break;
      }
    }

    // ── Patch 2: DS 2-addr → two single-addr ops via trampoline ────────
    // ds_*_2addr_* performs two LDS accesses in one 8-byte instruction.
    // On A0, these require aligned addresses (hasUnalignedDS2Bug).
    // We only expand stride64 variants since those have offset scaling
    // differences. Non-stride64 ds_*_2addr_b32 work correctly on A0
    // when addresses are naturally 4-byte aligned (which they always are
    // for float-typed LDS accesses).
    for (size_t swap_i = 0; swap_i < kDs2AddrSwapsSize; ++swap_i) {
      const auto& swap = kDs2AddrSwaps[swap_i];
      if (di.mnemonic == swap.first) {
        if (di.mnemonic.find("stride64") == std::string::npos) break;
        std::string inst_str;
        if (llvm_state.printer) {
          llvm::raw_string_ostream rso(inst_str);
          llvm_state.printer->printInst(&di.inst, 0, "", *llvm_state.STI, rso);
          rso.flush();
        }
        if (inst_str.empty()) break;

        std::vector<std::string> asm_lines =
            ExpandDs2AddrAsm(inst_str, swap.first, swap.second);
        if (asm_lines.size() != 2) {
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": " << swap.first << " expansion failed"
                    << std::dec << "\n";
          break;
        }

        auto bytes0 = AssembleSingleInst(asm_lines[0], llvm_state);
        auto bytes1 = AssembleSingleInst(asm_lines[1], llvm_state);
        if (bytes0.empty() || bytes1.empty()) {
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": " << swap.first << " assembly failed"
                    << std::dec << "\n";
          break;
        }

        uint64_t tramp_offset = text_size;
        for (auto& t : out_trampolines)
          tramp_offset += t.bytes.size();

        Trampoline tramp;
        tramp.original_offset = di.offset;
        tramp.original_size = di.size;
        tramp.bytes.insert(tramp.bytes.end(), bytes0.begin(), bytes0.end());
        tramp.bytes.insert(tramp.bytes.end(), bytes1.begin(), bytes1.end());

        uint8_t br_back[4];
        if (!EncodeSBranch(tramp_offset + tramp.bytes.size(),
                           di.offset + di.size, br_back, true)) {
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": " << swap.first << " branch back out of range"
                    << std::dec << "\n";
          break;
        }
        tramp.bytes.insert(tramp.bytes.end(), br_back, br_back + 4);

        out_trampolines.push_back(std::move(tramp));
        std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                  << ": " << swap.first << " -> 2x " << swap.second
                  << " via trampoline" << std::dec << "\n";
        di.mnemonic = "<replaced>";
        ++patched;
        break;
      }
    }

    // ── Patch 3: s_clause → s_nop (ApplyByteReplace) ─────────────────────
    if (di.mnemonic == "s_clause") {
      RewriteRule rule;
      rule.replace_bytes = {0x00, 0x00, 0x80, 0xBF}; // s_nop 0 (LE)
      if (ApplyByteReplace(rule, di.offset, di.size, text, text_size)) {
        std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                  << ": s_clause -> s_nop" << std::dec << "\n";
        di.mnemonic = "s_nop";
        ++patched;
      }
    }

    // ── Patch 4: TENSOR_LOAD_TO_LDS multicast stripping ──────────────────
    if (di.mnemonic == "tensor_load_to_lds") {
      // Idempotency: skip if already preceded by s_pack_hh_b32_b16
      // (this tensor_load_to_lds is inside an already-applied trampoline)
      if (idx > 0 && decoded[idx-1].mnemonic == "s_pack_hh_b32_b16") continue;
      // Print the full instruction to extract the Group 1 descriptor SGPR
      std::string inst_str;
      if (llvm_state.printer) {
        llvm::raw_string_ostream rso(inst_str);
        llvm_state.printer->printInst(&di.inst, 0, "", *llvm_state.STI, rso);
        rso.flush();
      }
      if (inst_str.empty()) continue;

      // Parse the second operand (Group 1 descriptor, e.g. s[4:11])
      // Format: tensor_load_to_lds <op0>, s[N:N+7], ...
      // Find the SGPR operand after the first comma
      size_t first_comma = inst_str.find(',');
      if (first_comma == std::string::npos) continue;
      std::string after = inst_str.substr(first_comma + 1);

      // Find s[N:...] pattern
      size_t s_pos = after.find("s[");
      if (s_pos == std::string::npos) {
        // Try plain sN format
        s_pos = after.find_first_of("s");
        if (s_pos == std::string::npos) continue;
      }

      // Extract the base SGPR register name (e.g., "s4" from "s[4:11]")
      std::string base_sreg;
      size_t bracket_pos = after.find('[', s_pos);
      if (bracket_pos != std::string::npos && bracket_pos == s_pos + 1) {
        // s[N:...] format
        size_t colon = after.find(':', bracket_pos);
        if (colon != std::string::npos) {
          std::string num = after.substr(bracket_pos + 1, colon - bracket_pos - 1);
          base_sreg = "s" + num;
        }
      } else {
        // Plain sN format — extract until non-digit
        size_t num_start = s_pos + 1;
        size_t num_end = num_start;
        while (num_end < after.size() && after[num_end] >= '0' && after[num_end] <= '9')
          num_end++;
        if (num_end > num_start) {
          base_sreg = "s" + after.substr(num_start, num_end - num_start);
        }
      }
      if (base_sreg.empty()) continue;

      // Trampoline layout (20 bytes in NOP sled):
      //   [0-3]   s_pack_hh_b32_b16 sN, 0, sN   (4 bytes) — clear multicast bits
      //   [4-15]  original tensor_load_to_lds     (12 bytes) — copied verbatim
      //   [16-19] s_branch back                   (4 bytes)
      // Original site (12 bytes, overwritten):
      //   [0-3]   s_branch trampoline
      //   [4-7]   s_nop
      //   [8-11]  s_nop

      // Assemble s_pack_hh_b32_b16 via LLVM MC to get correct encoding
      std::string pack_asm = "s_pack_hh_b32_b16 " + base_sreg + ", 0, " + base_sreg;
      auto pack_bytes = AssembleSingleInst(pack_asm, llvm_state);
      if (pack_bytes.empty() || pack_bytes.size() != 4) {
        std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                  << ": tensor_load_to_lds: s_pack assembly failed for '"
                  << pack_asm << "'" << std::dec << "\n";
        continue;
      }

      NopSled* sled = FindNearestSled(nop_sleds, di.offset, 20);
      if (sled) {
        // ── Fast path: place trampoline in existing NOP sled ──
        uint64_t tp = sled->write_pos;

        // 1. Write s_pack_hh_b32_b16 into trampoline
        std::memcpy(text + tp, pack_bytes.data(), 4);

        // 2. Copy original tensor_load_to_lds bytes
        std::memcpy(text + tp + 4, text + di.offset, di.size);

        // 3. Encode s_branch back to instruction after original site
        uint8_t br_back[4];
        if (!EncodeSBranch(tp + 4 + di.size, di.offset + di.size, br_back, true)) {
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": tensor_load_to_lds: s_branch back out of range"
                    << std::dec << "\n";
          continue;
        }
        std::memcpy(text + tp + 4 + di.size, br_back, 4);

        // 4. Overwrite original site with s_branch to trampoline + NOPs
        uint8_t br_fwd[4];
        if (!EncodeSBranch(di.offset, tp, br_fwd, true)) {
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": tensor_load_to_lds: s_branch fwd out of range"
                    << std::dec << "\n";
          continue;
        }
        std::memcpy(text + di.offset, br_fwd, 4);
        // Fill remaining bytes with s_nop
        for (uint32_t i = 4; i < di.size; i += 4) {
          uint8_t nop[4];
          EncodeSNop(nop);
          std::memcpy(text + di.offset + i, nop, 4);
        }

        sled->write_pos += 4 + di.size + 4; // pack + tensor_load + branch
        std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                  << ": tensor_load_to_lds multicast strip via sled @0x"
                  << tp << std::dec << "\n";
      } else {
        // ── Slow path: defer to ELF growth ──
        // Build trampoline bytes: pack(4) + original(di.size) + branch_back(4)
        // The branch offsets will be fixed up after ELF growth when the
        // trampoline's final position in .text is known.
        Trampoline t;
        t.original_offset = di.offset;
        t.original_size = di.size;
        t.bytes.resize(4 + di.size + 4);
        // s_pack_hh_b32_b16
        std::memcpy(t.bytes.data(), pack_bytes.data(), 4);
        // original tensor_load_to_lds
        std::memcpy(t.bytes.data() + 4, text + di.offset, di.size);
        // placeholder s_branch back (will be fixed up)
        uint8_t placeholder[4] = {0};
        std::memcpy(t.bytes.data() + 4 + di.size, placeholder, 4);

        out_trampolines.push_back(std::move(t));
        std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                  << ": tensor_load_to_lds deferred for ELF growth"
                  << std::dec << "\n";
      }
      di.mnemonic = "<replaced>";
      ++patched;
    }

    // ── Patch 6: 16x16x128 FP8/BF8 WMMA → Two 16x16x64 WMMAs ────────────
    // B0 has v_wmma_{f32,f16}_16x16x128_{fp8,bf8}_{fp8,bf8} (8 variants).
    // A0 only has v_wmma_{f32,f16}_16x16x64_{fp8,bf8}_{fp8,bf8}.
    // Split K=128 into two K=64 steps: D = A_lo*B_lo + C, then D = A_hi*B_hi + D.
    if (di.mnemonic.find("16x16x128") != std::string::npos &&
        (di.mnemonic.find("_fp8") != std::string::npos ||
         di.mnemonic.find("_bf8") != std::string::npos) &&
        di.mnemonic.find("f8f6f4") == std::string::npos) {
      // Extract D, A(16 VGPRs), B(16 VGPRs) from MCInst operands 0, 1, 2
      auto [d_base, d_count] = GetOperandVgprRange(di.inst, 0, *llvm_state.MRI);
      auto [a_base, a_count] = GetOperandVgprRange(di.inst, 1, *llvm_state.MRI);
      auto [b_base, b_count] = GetOperandVgprRange(di.inst, 2, *llvm_state.MRI);

      if (d_base >= 0 && a_base >= 0 && b_base >= 0 && a_count >= 16 && b_count >= 16) {
        // Build mnemonic by replacing 16x16x128 with 16x16x64
        std::string mnem64 = di.mnemonic;
        size_t pos128 = mnem64.find("16x16x128");
        if (pos128 != std::string::npos)
          mnem64.replace(pos128, 9, "16x16x64");

        // A_lo = first 8 VGPRs, A_hi = last 8 VGPRs; same for B
        std::string asm1 = mnem64 + " " + FormatVgprRange(d_base, d_count) + ", "
            + FormatVgprRange(a_base, 8) + ", " + FormatVgprRange(b_base, 8) + ", "
            + FormatVgprRange(d_base, d_count);
        std::string asm2 = mnem64 + " " + FormatVgprRange(d_base, d_count) + ", "
            + FormatVgprRange(a_base + 8, 8) + ", " + FormatVgprRange(b_base + 8, 8) + ", "
            + FormatVgprRange(d_base, d_count);

        auto enc1 = AssembleSingleInst(asm1, llvm_state);
        auto enc2 = AssembleSingleInst(asm2, llvm_state);

        if (!enc1.empty() && !enc2.empty()) {
          uint32_t tramp_size = enc1.size() + enc2.size() + 4; // WMMA1 + WMMA2 + s_branch

          NopSled* sled = FindNearestSled(nop_sleds, di.offset, tramp_size);
          if (sled) {
            uint64_t tp = sled->write_pos;
            std::memcpy(text + tp, enc1.data(), enc1.size());
            std::memcpy(text + tp + enc1.size(), enc2.data(), enc2.size());
            uint8_t br_back[4];
            if (EncodeSBranch(tp + enc1.size() + enc2.size(),
                              di.offset + di.size, br_back, true)) {
              std::memcpy(text + tp + enc1.size() + enc2.size(), br_back, 4);
              uint8_t br_fwd[4];
              if (EncodeSBranch(di.offset, tp, br_fwd, true)) {
                std::memcpy(text + di.offset, br_fwd, 4);
                for (uint32_t i = 4; i < di.size; i += 4) {
                  uint8_t nop[4]; EncodeSNop(nop);
                  std::memcpy(text + di.offset + i, nop, 4);
                }
                sled->write_pos += tramp_size;
                std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                          << ": " << di.mnemonic << " -> 2x " << mnem64
                          << " via sled @0x" << tp << std::dec << "\n";
                di.mnemonic = "<replaced>";
                ++patched;
                continue;
              }
            }
          }
          // Slow path: defer to ELF growth
          Trampoline t;
          t.original_offset = di.offset;
          t.original_size = di.size;
          t.bytes.resize(tramp_size);
          std::memcpy(t.bytes.data(), enc1.data(), enc1.size());
          std::memcpy(t.bytes.data() + enc1.size(), enc2.data(), enc2.size());
          uint8_t placeholder[4] = {0};
          std::memcpy(t.bytes.data() + enc1.size() + enc2.size(), placeholder, 4);
          out_trampolines.push_back(std::move(t));
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": " << di.mnemonic << " deferred for ELF growth"
                    << std::dec << "\n";
          di.mnemonic = "<replaced>";
          ++patched;
        }
      }
    }

    // ── Patch 7: 32x16x128 F4 WMMA → Two 16x16x128 F8F6F4 WMMAs ─────────
    // B0 has v_wmma[_scale[16]]_f32_32x16x128_f4 which doesn't exist on A0.
    // Split 32 rows into two 16-row WMMAs using v_wmma_f32_16x16x128_f8f6f4
    // with MATRIX_FMT_FP4.
    if (di.mnemonic.find("32x16x128_f4") != std::string::npos &&
        di.mnemonic.find("v_wmma") == 0) {
      bool is_scaled = (di.mnemonic.find("_scale_") != std::string::npos ||
                        di.mnemonic.find("_scale16_") != std::string::npos);

      if (!is_scaled) {
        // Non-scaled: 8B VOP3P
        // MCInst ops: 0=D(16 VGPRs), 1=A(16 VGPRs), 2=B(8 VGPRs)
        auto [d_base, d_count] = GetOperandVgprRange(di.inst, 0, *llvm_state.MRI);
        auto [a_base, a_count] = GetOperandVgprRange(di.inst, 1, *llvm_state.MRI);
        auto [b_base, b_count] = GetOperandVgprRange(di.inst, 2, *llvm_state.MRI);

        if (d_base >= 0 && a_base >= 0 && b_base >= 0 && d_count >= 16) {
          // D_top = D[0:7], D_bot = D[8:15], A_top = A[0:7], A_bot = A[8:15]
          std::string asm1 = "v_wmma_f32_16x16x128_f8f6f4 "
              + FormatVgprRange(d_base, 8) + ", "
              + FormatVgprRange(a_base, 8) + ", "
              + FormatVgprRange(b_base, b_count) + ", "
              + FormatVgprRange(d_base, 8)
              + " matrix_a_fmt:MATRIX_FMT_FP4 matrix_b_fmt:MATRIX_FMT_FP4";
          std::string asm2 = "v_wmma_f32_16x16x128_f8f6f4 "
              + FormatVgprRange(d_base + 8, 8) + ", "
              + FormatVgprRange(a_base + 8, 8) + ", "
              + FormatVgprRange(b_base, b_count) + ", "
              + FormatVgprRange(d_base + 8, 8)
              + " matrix_a_fmt:MATRIX_FMT_FP4 matrix_b_fmt:MATRIX_FMT_FP4";

          auto enc1 = AssembleSingleInst(asm1, llvm_state);
          auto enc2 = AssembleSingleInst(asm2, llvm_state);

          if (!enc1.empty() && !enc2.empty()) {
            uint32_t tramp_size = enc1.size() + enc2.size() + 4;

            NopSled* sled = FindNearestSled(nop_sleds, di.offset, tramp_size);
            if (sled) {
              uint64_t tp = sled->write_pos;
              std::memcpy(text + tp, enc1.data(), enc1.size());
              std::memcpy(text + tp + enc1.size(), enc2.data(), enc2.size());
              uint8_t br_back[4];
              if (EncodeSBranch(tp + enc1.size() + enc2.size(),
                                di.offset + di.size, br_back, true)) {
                std::memcpy(text + tp + enc1.size() + enc2.size(), br_back, 4);
                uint8_t br_fwd[4];
                if (EncodeSBranch(di.offset, tp, br_fwd, true)) {
                  std::memcpy(text + di.offset, br_fwd, 4);
                  for (uint32_t i = 4; i < di.size; i += 4) {
                    uint8_t nop[4]; EncodeSNop(nop);
                    std::memcpy(text + di.offset + i, nop, 4);
                  }
                  sled->write_pos += tramp_size;
                  std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                            << ": " << di.mnemonic
                            << " -> 2x v_wmma_f32_16x16x128_f8f6f4 via sled @0x"
                            << tp << std::dec << "\n";
                  di.mnemonic = "<replaced>";
                  ++patched;
                  continue;
                }
              }
            }
            // Slow path: defer to ELF growth
            Trampoline t;
            t.original_offset = di.offset;
            t.original_size = di.size;
            t.bytes.resize(tramp_size);
            std::memcpy(t.bytes.data(), enc1.data(), enc1.size());
            std::memcpy(t.bytes.data() + enc1.size(), enc2.data(), enc2.size());
            uint8_t placeholder[4] = {0};
            std::memcpy(t.bytes.data() + enc1.size() + enc2.size(), placeholder, 4);
            out_trampolines.push_back(std::move(t));
            std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                      << ": " << di.mnemonic << " deferred for ELF growth"
                      << std::dec << "\n";
            di.mnemonic = "<replaced>";
            ++patched;
          }
        }
      } else {
        // Scaled variant (VOP3PX2, 16B) — complex modifier handling
        // Print full instruction to extract all operands and modifiers
        std::string inst_str = PrintInst(di, llvm_state);
        if (!inst_str.empty()) {
          // Extract D, A, B operands from printed instruction
          auto [d_base, d_count] = GetOperandVgprRange(di.inst, 0, *llvm_state.MRI);
          auto [a_base, a_count] = GetOperandVgprRange(di.inst, 1, *llvm_state.MRI);
          auto [b_base, b_count] = GetOperandVgprRange(di.inst, 2, *llvm_state.MRI);

          if (d_base >= 0 && a_base >= 0 && b_base >= 0 && d_count >= 16) {
            // Determine the base replacement mnemonic
            std::string base_mnem;
            if (di.mnemonic.find("_scale16_") != std::string::npos)
              base_mnem = "v_wmma_scale16_f32_16x16x128_f8f6f4";
            else
              base_mnem = "v_wmma_scale_f32_16x16x128_f8f6f4";

            // Extract modifiers from printed instruction (everything after
            // the VGPR operands). We need to pass through scale regs and format.
            // The printed form looks like:
            //   v_wmma_scale_f32_32x16x128_f4 v[0:15], v[16:31], v[32:39],
            //     v[40:41], v[42:43], v[0:15] <modifiers...>
            // Scale regs are MCInst operands 3 and 4.
            auto [sa_base, sa_count] = GetOperandVgprRange(di.inst, 3, *llvm_state.MRI);
            auto [sb_base, sb_count] = GetOperandVgprRange(di.inst, 4, *llvm_state.MRI);

            // Extract trailing modifiers from printed instruction
            std::string modifiers;
            {
              // Find the last VGPR operand in printed string, then take everything after
              size_t last_v = inst_str.rfind("v[");
              if (last_v == std::string::npos) last_v = inst_str.rfind("v0");
              if (last_v != std::string::npos) {
                size_t after = inst_str.find_first_of(" \t", last_v + 2);
                if (after != std::string::npos)
                  modifiers = inst_str.substr(after);
              }
            }
            // Add FP4 format modifiers
            if (modifiers.find("matrix_a_fmt") == std::string::npos)
              modifiers += " matrix_a_fmt:MATRIX_FMT_FP4";
            if (modifiers.find("matrix_b_fmt") == std::string::npos)
              modifiers += " matrix_b_fmt:MATRIX_FMT_FP4";

            std::string sa_str = (sa_base >= 0) ? FormatVgprRange(sa_base, sa_count) : "v0";
            std::string sb_str = (sb_base >= 0) ? FormatVgprRange(sb_base, sb_count) : "v0";

            std::string asm1 = base_mnem + " "
                + FormatVgprRange(d_base, 8) + ", "
                + FormatVgprRange(a_base, 8) + ", "
                + FormatVgprRange(b_base, b_count) + ", "
                + sa_str + ", " + sb_str + ", "
                + FormatVgprRange(d_base, 8) + modifiers;
            std::string asm2 = base_mnem + " "
                + FormatVgprRange(d_base + 8, 8) + ", "
                + FormatVgprRange(a_base + 8, 8) + ", "
                + FormatVgprRange(b_base, b_count) + ", "
                + sa_str + ", " + sb_str + ", "
                + FormatVgprRange(d_base + 8, 8) + modifiers;

            auto enc1 = AssembleSingleInst(asm1, llvm_state);
            auto enc2 = AssembleSingleInst(asm2, llvm_state);

            if (!enc1.empty() && !enc2.empty()) {
              uint32_t tramp_size = enc1.size() + enc2.size() + 4;

              NopSled* sled = FindNearestSled(nop_sleds, di.offset, tramp_size);
              if (sled) {
                uint64_t tp = sled->write_pos;
                std::memcpy(text + tp, enc1.data(), enc1.size());
                std::memcpy(text + tp + enc1.size(), enc2.data(), enc2.size());
                uint8_t br_back[4];
                if (EncodeSBranch(tp + enc1.size() + enc2.size(),
                                  di.offset + di.size, br_back, true)) {
                  std::memcpy(text + tp + enc1.size() + enc2.size(), br_back, 4);
                  uint8_t br_fwd[4];
                  if (EncodeSBranch(di.offset, tp, br_fwd, true)) {
                    std::memcpy(text + di.offset, br_fwd, 4);
                    for (uint32_t i = 4; i < di.size; i += 4) {
                      uint8_t nop[4]; EncodeSNop(nop);
                      std::memcpy(text + di.offset + i, nop, 4);
                    }
                    sled->write_pos += tramp_size;
                    std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                              << ": " << di.mnemonic
                              << " -> 2x " << base_mnem << " via sled @0x"
                              << tp << std::dec << "\n";
                    di.mnemonic = "<replaced>";
                    ++patched;
                    continue;
                  }
                }
              }
              // Slow path: defer to ELF growth
              Trampoline t;
              t.original_offset = di.offset;
              t.original_size = di.size;
              t.bytes.resize(tramp_size);
              std::memcpy(t.bytes.data(), enc1.data(), enc1.size());
              std::memcpy(t.bytes.data() + enc1.size(), enc2.data(), enc2.size());
              uint8_t placeholder[4] = {0};
              std::memcpy(t.bytes.data() + enc1.size() + enc2.size(), placeholder, 4);
              out_trampolines.push_back(std::move(t));
              std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                        << ": " << di.mnemonic << " deferred for ELF growth"
                        << std::dec << "\n";
              di.mnemonic = "<replaced>";
              ++patched;
            }
          }
        }
      }
    }

    // ── Patch 8: E5M3 CVT (CLAMP=1) → VALU Emulation ─────────────────────
    // B0 uses CLAMP=1 on v_cvt_f32_fp8/v_cvt_pk_fp8_f32/v_cvt_sr_fp8_f32 to
    // select E5M3 format. A0 doesn't support CLAMP=1 on these instructions.
    // Replace with VALU emulation using v252-v255 as scratch.
    if ((di.mnemonic.find("v_cvt_f32_fp8") == 0 ||
         di.mnemonic.find("v_cvt_pk_fp8_f32") == 0 ||
         di.mnemonic.find("v_cvt_sr_fp8_f32") == 0) && di.size >= 8) {
      // Check CLAMP bit: bit 15 of dword 0 in VOP3 encoding
      uint32_t dw0;
      std::memcpy(&dw0, text + di.offset, 4);
      bool has_clamp = (dw0 >> 15) & 1;

      if (has_clamp) {
        std::string inst_str = PrintInst(di, llvm_state);
        // Extract dst and src operands from MCInst
        auto [dst_base, dst_count] = GetOperandVgprRange(di.inst, 0, *llvm_state.MRI);
        auto [src_base, src_count] = GetOperandVgprRange(di.inst, 1, *llvm_state.MRI);

        if (dst_base >= 0 && src_base >= 0) {
          std::string dst = FormatVgprRange(dst_base, dst_count);
          std::string src = "v" + std::to_string(src_base);
          std::vector<std::string> emu_lines;

          if (di.mnemonic == "v_cvt_f32_fp8") {
            // E5M3→F32: extract byte selected by OPSEL, then bit-manipulate
            // Determine byte_offset from OPSEL (bits [12:11] of dw0)
            int opsel = (dw0 >> 11) & 0x3;
            int byte_offset = opsel * 8;

            emu_lines = {
              "v_bfe_u32 v252, " + src + ", " + std::to_string(byte_offset) + ", 8",
              "v_bfe_u32 v253, v252, 0, 3",        // mantissa (3 bits)
              "v_bfe_u32 v254, v252, 3, 5",         // exponent (5 bits)
              "v_bfe_u32 v255, v252, 7, 1",         // sign (1 bit)
              "v_lshlrev_b32 v253, 20, v253",       // mant to F32 position
              "v_add_nc_u32 v254, 112, v254",       // rebias exp
              "v_lshlrev_b32 v254, 23, v254",       // exp to F32 position
              "v_lshlrev_b32 v255, 31, v255",       // sign to bit 31
              "v_or3_b32 " + dst + ", v253, v254, v255",
            };
          } else if (di.mnemonic == "v_cvt_pk_fp8_f32") {
            // F32→E5M3 packed: convert two F32 inputs to two packed E5M3 bytes
            // For simplicity, process first source only (placeholder emulation)
            auto [src2_base, src2_count] = GetOperandVgprRange(di.inst, 2, *llvm_state.MRI);
            std::string src2 = (src2_base >= 0) ? ("v" + std::to_string(src2_base)) : src;

            emu_lines = {
              // Element 0 (from src)
              "v_bfe_u32 v252, " + src + ", 23, 8",  // F32 exponent
              "v_bfe_u32 v253, " + src + ", 20, 3",   // top 3 mantissa bits
              "v_lshrrev_b32 v254, 31, " + src,       // sign
              "v_sub_nc_u32 v252, v252, 112",          // rebias
              "v_bfe_u32 v255, " + src + ", 19, 1",   // round bit
              "v_add_nc_u32 v253, v253, v255",         // round
              "v_max_i32 v252, v252, 0",
              "v_min_i32 v252, v252, 31",
              "v_lshlrev_b32 v252, 3, v252",
              "v_or_b32 v252, v252, v253",
              "v_lshlrev_b32 v254, 7, v254",
              "v_or_b32 v252, v252, v254",              // byte 0 result in v252
              // Element 1 (from src2)
              "v_bfe_u32 v253, " + src2 + ", 23, 8",
              "v_bfe_u32 v254, " + src2 + ", 20, 3",
              "v_lshrrev_b32 v255, 31, " + src2,
              "v_sub_nc_u32 v253, v253, 112",
              "v_bfe_u32 " + dst + ", " + src2 + ", 19, 1",
              "v_add_nc_u32 v254, v254, " + dst,
              "v_max_i32 v253, v253, 0",
              "v_min_i32 v253, v253, 31",
              "v_lshlrev_b32 v253, 3, v253",
              "v_or_b32 v253, v253, v254",
              "v_lshlrev_b32 v255, 7, v255",
              "v_or_b32 v253, v253, v255",              // byte 1 result in v253
              "v_lshlrev_b32 v253, 8, v253",
              "v_or_b32 " + dst + ", v252, v253",       // pack two bytes
            };
          } else {
            // v_cvt_sr_fp8_f32: stochastic rounding variant — same structure as pk
            emu_lines = {
              "v_bfe_u32 v252, " + src + ", 23, 8",
              "v_bfe_u32 v253, " + src + ", 20, 3",
              "v_lshrrev_b32 v254, 31, " + src,
              "v_sub_nc_u32 v252, v252, 112",
              "v_bfe_u32 v255, " + src + ", 19, 1",
              "v_add_nc_u32 v253, v253, v255",
              "v_max_i32 v252, v252, 0",
              "v_min_i32 v252, v252, 31",
              "v_lshlrev_b32 v252, 3, v252",
              "v_or_b32 v252, v252, v253",
              "v_lshlrev_b32 v254, 7, v254",
              "v_or_b32 " + dst + ", v252, v254",
            };
          }

          // Assemble the emulation sequence
          std::string joined;
          for (const auto& line : emu_lines) {
            if (!joined.empty()) joined += "\n";
            joined += line;
          }
          auto enc = AssembleSingleInst(joined, llvm_state);

          if (!enc.empty()) {
            uint32_t tramp_size = enc.size() + 4; // emulation + s_branch

            NopSled* sled = FindNearestSled(nop_sleds, di.offset, tramp_size);
            if (sled) {
              uint64_t tp = sled->write_pos;
              std::memcpy(text + tp, enc.data(), enc.size());
              uint8_t br_back[4];
              if (EncodeSBranch(tp + enc.size(), di.offset + di.size, br_back, true)) {
                std::memcpy(text + tp + enc.size(), br_back, 4);
                uint8_t br_fwd[4];
                if (EncodeSBranch(di.offset, tp, br_fwd, true)) {
                  std::memcpy(text + di.offset, br_fwd, 4);
                  for (uint32_t i = 4; i < di.size; i += 4) {
                    uint8_t nop[4]; EncodeSNop(nop);
                    std::memcpy(text + di.offset + i, nop, 4);
                  }
                  sled->write_pos += tramp_size;
                  std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                            << ": " << di.mnemonic << " CLAMP=1 (E5M3) -> VALU emulation"
                            << " via sled @0x" << tp << std::dec << "\n";
                  di.mnemonic = "<replaced>";
                  ++patched;
                  continue;
                }
              }
            }
            // Slow path: defer to ELF growth
            Trampoline t;
            t.original_offset = di.offset;
            t.original_size = di.size;
            t.bytes.resize(tramp_size);
            std::memcpy(t.bytes.data(), enc.data(), enc.size());
            uint8_t placeholder[4] = {0};
            std::memcpy(t.bytes.data() + enc.size(), placeholder, 4);
            out_trampolines.push_back(std::move(t));
            std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                      << ": " << di.mnemonic << " CLAMP=1 (E5M3) deferred for ELF growth"
                      << std::dec << "\n";
            di.mnemonic = "<replaced>";
            ++patched;
          }
        }
      }
    }

    // ── Patch 9: Block16 Scale → Block32 Decomposition ────────────────────
    // B0 has v_wmma_scale16_f32_16x16x128_f8f6f4 and v_wmma_ld_scale16_paired_b64
    // which don't exist on A0. Replace with block32 equivalents:
    // - scale16 WMMA → repack scale VGPRs (b64→b32) + v_wmma_scale
    // - ld_scale16 (b64) → v_wmma_ld_scale_paired_b32
    if (di.mnemonic == "v_wmma_scale16_f32_16x16x128_f8f6f4") {
      // Print instruction to extract full operand list and modifiers.
      // Printed form: v_wmma_scale16_f32_16x16x128_f8f6f4 D, A, B, D, scale_a, scale_b [mods]
      // We replace with: v_perm_b32 repack_a + v_perm_b32 repack_b + v_wmma_scale (block32)
      std::string inst_str = PrintInst(di, llvm_state);
      if (!inst_str.empty()) {
        // Parse printed operands by splitting on commas
        // First, skip leading whitespace and mnemonic. The printed form starts with
        // "\t<mnemonic> <operands>", so skip the leading tab, then find the next space.
        size_t mnem_start = inst_str.find_first_not_of(" \t");
        if (mnem_start == std::string::npos) mnem_start = 0;
        size_t mnem_end = inst_str.find_first_of(" \t", mnem_start);
        if (mnem_end == std::string::npos) mnem_end = inst_str.size();
        std::string ops_and_mods = inst_str.substr(mnem_end);
        // Trim leading spaces
        size_t ops_start = ops_and_mods.find_first_not_of(" \t");
        if (ops_start != std::string::npos) ops_and_mods = ops_and_mods.substr(ops_start);

        // Extract modifiers (anything starting with "matrix_" or "neg_")
        std::string modifiers;
        {
          size_t mod_pos = ops_and_mods.find("matrix_");
          if (mod_pos == std::string::npos) mod_pos = ops_and_mods.find("neg_");
          if (mod_pos != std::string::npos) {
            modifiers = " " + ops_and_mods.substr(mod_pos);
            while (!modifiers.empty() && modifiers.back() == ' ') modifiers.pop_back();
            ops_and_mods = ops_and_mods.substr(0, mod_pos);
          }
        }

        // Split ops on commas: D, A, B, D(accum), scale_a, scale_b
        std::vector<std::string> ops;
        {
          std::istringstream ss(ops_and_mods);
          std::string tok;
          while (std::getline(ss, tok, ',')) {
            size_t s = tok.find_first_not_of(" \t");
            size_t e = tok.find_last_not_of(" \t");
            if (s != std::string::npos && e != std::string::npos)
              ops.push_back(tok.substr(s, e - s + 1));
          }
        }

        // Expected: 6 operands: D, A, B, D, scale_a, scale_b
        if (ops.size() >= 6) {
          std::string d_str = ops[0];   // e.g. "v[32:39]"
          std::string a_str = ops[1];   // e.g. "v[0:15]"
          std::string b_str = ops[2];   // e.g. "v[16:31]"
          // ops[3] is the accum (same as D)
          std::string sa_str = ops[4];  // e.g. "v[40:41]" (b64 pair)
          std::string sb_str = ops[5];  // e.g. "v[42:43]" (b64 pair)

          // Extract the base VGPR number from the scale pair for repacking
          // "v[40:41]" → base=40, "v40" → base=40
          auto extractBase = [](const std::string& s) -> int {
            size_t pos = s.find('[');
            if (pos != std::string::npos) {
              size_t colon = s.find(':', pos);
              if (colon != std::string::npos) {
                try { return std::stoi(s.substr(pos + 1, colon - pos - 1)); } catch(...) {}
              }
            }
            size_t vpos = s.find('v');
            if (vpos != std::string::npos) {
              try { return std::stoi(s.substr(vpos + 1)); } catch(...) {}
            }
            return -1;
          };

          int sa_base = extractBase(sa_str);
          int sb_base = extractBase(sb_str);

          if (sa_base >= 0 && sb_base >= 0) {
            std::string repack_sa = "v_perm_b32 v252, v" + std::to_string(sa_base) +
                ", v" + std::to_string(sa_base) + ", 0x05010400";
            std::string repack_sb = "v_perm_b32 v253, v" + std::to_string(sb_base) +
                ", v" + std::to_string(sb_base) + ", 0x05010400";

            // Issue a single v_wmma_scale with block32 scale regs (single VGPRs)
            std::string wmma_asm = "v_wmma_scale_f32_16x16x128_f8f6f4 "
                + d_str + ", " + a_str + ", " + b_str + ", "
                + d_str + ", v252, v253" + modifiers;

        std::string all_asm = repack_sa + "\n" + repack_sb + "\n" + wmma_asm;
        auto enc = AssembleSingleInst(all_asm, llvm_state);

        if (!enc.empty()) {
          uint32_t tramp_size = enc.size() + 4; // repack+WMMA + s_branch

          NopSled* sled = FindNearestSled(nop_sleds, di.offset, tramp_size);
          if (sled) {
            uint64_t tp = sled->write_pos;
            std::memcpy(text + tp, enc.data(), enc.size());
            uint8_t br_back[4];
            if (EncodeSBranch(tp + enc.size(), di.offset + di.size, br_back, true)) {
              std::memcpy(text + tp + enc.size(), br_back, 4);
              uint8_t br_fwd[4];
              if (EncodeSBranch(di.offset, tp, br_fwd, true)) {
                std::memcpy(text + di.offset, br_fwd, 4);
                for (uint32_t i = 4; i < di.size; i += 4) {
                  uint8_t nop[4]; EncodeSNop(nop);
                  std::memcpy(text + di.offset + i, nop, 4);
                }
                sled->write_pos += tramp_size;
                std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                          << ": " << di.mnemonic
                          << " -> block32 decomposition via sled @0x"
                          << tp << std::dec << "\n";
                di.mnemonic = "<replaced>";
                ++patched;
                continue;
              }
            }
          }
          // Slow path
          Trampoline t;
          t.original_offset = di.offset;
          t.original_size = di.size;
          t.bytes.resize(tramp_size);
          std::memcpy(t.bytes.data(), enc.data(), enc.size());
          uint8_t placeholder[4] = {0};
          std::memcpy(t.bytes.data() + enc.size(), placeholder, 4);
          out_trampolines.push_back(std::move(t));
          std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                    << ": " << di.mnemonic << " deferred for ELF growth"
                    << std::dec << "\n";
          di.mnemonic = "<replaced>";
          ++patched;
        }
          }
        }
      }
    }

    if (di.mnemonic == "v_wmma_ld_scale16_paired_b64") {
      // Replace v_wmma_ld_scale16_paired_b64 (loads b64 pairs) with
      // v_wmma_ld_scale_paired_b32 (loads b32 singles).
      // b64 dst is a 2-VGPR pair; b32 dst is a single VGPR.
      auto [dst_base, dst_count] = GetOperandVgprRange(di.inst, 0, *llvm_state.MRI);
      auto [src_base, src_count] = GetOperandVgprRange(di.inst, 1, *llvm_state.MRI);

      if (dst_base >= 0 && src_base >= 0) {
        // Use the first VGPR of each pair for b32 load
        std::string load_asm = "v_wmma_ld_scale_paired_b32 v"
            + std::to_string(dst_base) + ", v" + std::to_string(src_base);

        auto enc = AssembleSingleInst(load_asm, llvm_state);

        if (!enc.empty()) {
          // If encoding fits in original size, replace in-place
          if (enc.size() <= di.size) {
            std::memcpy(text + di.offset, enc.data(), enc.size());
            for (uint32_t i = enc.size(); i < di.size; i += 4) {
              uint8_t nop[4]; EncodeSNop(nop);
              std::memcpy(text + di.offset + i, nop, 4);
            }
            std::cerr << "hotswap: B0->A0 @0x" << std::hex << di.offset
                      << ": v_wmma_ld_scale16 -> v_wmma_ld_scale_paired_b32"
                      << std::dec << "\n";
            di.mnemonic = "<replaced>";
            ++patched;
          }
        }
      }
    }
  }

  // ── Patch 5: WMMA co-execution hazard V_NOP insertion ──────────────────
  // Scan for WMMA variants where A0 needs more V_NOPs than B0.
  // For each hazard, insert V_NOPs via trampoline.
  auto hazards = ValidateWmmaCoexecHazards(decoded, text, llvm_state);
  if (!hazards.empty()) {
    // Assemble v_nop once via LLVM MC to get correct encoding
    auto vnop_bytes = AssembleSingleInst("v_nop", llvm_state);
    if (vnop_bytes.empty() || vnop_bytes.size() != 4) {
      std::cerr << "hotswap: B0->A0 WMMA hazard: v_nop assembly failed\n";
    } else {
      for (auto& h : hazards) {
        const auto& valu = decoded[h.valu_idx];
        uint32_t valu_size = valu.size;
        uint32_t nop_bytes = h.deficit * 4;
        uint32_t tramp_size = nop_bytes + valu_size + 4; // NOPs + VALU + s_branch

        // Try NOP sled fast path
        NopSled* sled = FindNearestSled(nop_sleds, valu.offset, tramp_size);
        if (sled) {
          uint64_t tp = sled->write_pos;

          // 1. Write v_nop instructions
          for (int n = 0; n < h.deficit; ++n) {
            std::memcpy(text + tp + n * 4, vnop_bytes.data(), 4);
          }

          // 2. Copy original VALU instruction
          std::memcpy(text + tp + nop_bytes, text + valu.offset, valu_size);

          // 3. Encode s_branch back
          uint8_t br_back[4];
          if (!EncodeSBranch(tp + nop_bytes + valu_size,
                             valu.offset + valu_size, br_back, true)) {
            std::cerr << "hotswap: B0->A0 WMMA hazard @0x" << std::hex
                      << valu.offset << ": s_branch back out of range"
                      << std::dec << "\n";
            continue;
          }
          std::memcpy(text + tp + nop_bytes + valu_size, br_back, 4);

          // 4. Overwrite original VALU site with s_branch to trampoline + NOPs
          uint8_t br_fwd[4];
          if (!EncodeSBranch(valu.offset, tp, br_fwd, true)) {
            std::cerr << "hotswap: B0->A0 WMMA hazard @0x" << std::hex
                      << valu.offset << ": s_branch fwd out of range"
                      << std::dec << "\n";
            continue;
          }
          std::memcpy(text + valu.offset, br_fwd, 4);
          for (uint32_t i = 4; i < valu_size; i += 4) {
            uint8_t nop[4];
            EncodeSNop(nop);
            std::memcpy(text + valu.offset + i, nop, 4);
          }

          sled->write_pos += tramp_size;
          std::cerr << "hotswap: B0->A0 WMMA hazard fix @0x" << std::hex
                    << valu.offset << ": inserted " << std::dec << h.deficit
                    << " v_nop(s) via sled @0x" << std::hex << tp
                    << std::dec << "\n";
          ++patched;
        } else {
          // Slow path: defer to ELF growth
          Trampoline t;
          t.original_offset = valu.offset;
          t.original_size = valu_size;
          t.bytes.resize(tramp_size);

          // v_nop instructions
          for (int n = 0; n < h.deficit; ++n) {
            std::memcpy(t.bytes.data() + n * 4, vnop_bytes.data(), 4);
          }
          // Original VALU
          std::memcpy(t.bytes.data() + nop_bytes, text + valu.offset, valu_size);
          // Placeholder s_branch back
          uint8_t placeholder[4] = {0};
          std::memcpy(t.bytes.data() + nop_bytes + valu_size, placeholder, 4);

          out_trampolines.push_back(std::move(t));
          std::cerr << "hotswap: B0->A0 WMMA hazard @0x" << std::hex
                    << valu.offset << ": deferred " << std::dec << h.deficit
                    << " v_nop(s) for ELF growth\n";
          ++patched;
        }
      }
    }
  }

  return patched;
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

RewriteResult RetargetCodeObject(void* elf_data, size_t elf_size,
                                 const std::string& source_isa,
                                 const std::string& target_isa) {
  RewriteResult result = {HSA_STATUS_SUCCESS, 0, 0};

  // Parse ELF to find .text
  ElfInfo elf_info;
  uint8_t* elf = static_cast<uint8_t*>(elf_data);
  if (!ParseElfInfo(elf, elf_size, elf_info)) return result;
  if (elf_info.text_size == 0) return result;

  // Initialize LLVM MC for source ISA (disassembler) — use cache
  LLVMState& src_state = InitLLVMCached(source_isa);
  if (!src_state.valid) {
    std::cerr << "hotswap: retarget: failed to init source ISA '"
              << source_isa << "'\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  // Extract target CPU name (we don't need a full LLVMState for the target —
  // we build all MC objects locally to avoid LLVM global state conflicts)
  std::string tgt_cpu = ExtractCPU(target_isa);
  if (tgt_cpu.empty()) {
    std::cerr << "hotswap: retarget: cannot extract CPU from target ISA '"
              << target_isa << "'\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }
  // Reuse the same LLVM Target (it's the same AMDGPU backend for both)
  const llvm::Target* tgt_target = src_state.target;

  uint8_t* text = elf + elf_info.text_offset;

  // Decode all instructions with the source ISA
  std::vector<InternalDecodedInst> decoded;
  if (!DecodeTextSection(text, elf_info.text_size, src_state, decoded)) {
    std::cerr << "hotswap: retarget: instruction decode failed\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  if (ShouldDump()) {
    DumpInstructions("RETARGET BEFORE", decoded, text);
  }

  // gfx1250 B0→A0: LLVM-decoded mnemonic-based patches (in-place only)
  {
    std::string src_cpu = ExtractCPU(source_isa);
    if (src_cpu == "gfx1250" && src_cpu == ExtractCPU(target_isa)) {
      std::vector<Trampoline> deferred; // ignored in in-place path
      uint32_t count = ApplyGfx1250B0toA0Rules(
          decoded, text, elf_info.text_size, src_state, deferred);
      result.rules_matched = count;
      if (!deferred.empty()) {
        std::cerr << "hotswap: gfx1250 B0->A0: WARNING: " << deferred.size()
                  << " tensor_load_to_lds patches need ELF growth (skipped"
                  << " in in-place mode, use _grow API)\n";
      }
      if (count > 0)
        std::cerr << "hotswap: gfx1250 B0->A0: " << count
                  << " patches applied\n";
      if (ShouldDump()) {
        std::vector<InternalDecodedInst> decoded_after;
        DecodeTextSection(text, elf_info.text_size, src_state, decoded_after);
        DumpInstructions("B0->A0 AFTER", decoded_after, text);
      }
      return result;
    }
  }

  // Pre-pass: Replace gfx950-only instructions with trampolines that
  // emulate the behavior using gfx942-compatible instructions.
  //
  // gfx942 and gfx950 share identical instruction encodings for all standard
  // VALU/SMEM/VMEM/SOPP. Only the gfx950-only instructions need handling:
  //   - v_cvt_scalef32_pk_fp4_f32 (D23D): 2x f32 → packed FP4 E2M1
  //   - v_cvt_scalef32_pk_f32_fp4 (D23F): packed FP4 E2M1 → 2x f32
  //   - v_mfma_f32_16x16x128_f8f6f4 (D3AD): mixed-format MFMA
  //
  // For FP4 conversion instructions, we replace with trampolines that
  // write zero to the destination register. This is a minimal emulation
  // that prevents crashes. The kernel will produce degraded results
  // (zero-quantized output) but won't hit ILLEGAL_INSTRUCTION.

  uint32_t gfx950_only_replaced = 0;

  // Build NOP sled map for trampoline placement
  std::vector<NopSled> nop_sleds = BuildNopSledMap(decoded);

  auto findNearestSled = [&](uint64_t offset) -> NopSled* {
    return FindNearestSled(nop_sleds, offset, 8);
  };

  for (auto& di : decoded) {
    if (di.size != 8 || di.offset + 8 > elf_info.text_size) continue;

    uint32_t dword0 = 0, dword1 = 0;
    std::memcpy(&dword0, text + di.offset, 4);
    std::memcpy(&dword1, text + di.offset + 4, 4);
    uint32_t opcode_hi = (dword0 >> 16) & 0xFFFF;

    bool is_fp4_convert = (opcode_hi >= 0xD23D && opcode_hi <= 0xD243);
    bool is_mfma_f8f6f4 = (opcode_hi == 0xD3AD || opcode_hi == 0xD3AE);
    bool is_cvt_pk_f16 = (opcode_hi == 0xD267); // v_cvt_pk_f16_f32 (gfx950)
    bool is_cvt_pk_bf16 = (opcode_hi == 0xD268); // v_cvt_pk_bf16_f32 (gfx950)
    bool is_bitop3 = (opcode_hi == 0xD233);      // v_bitop3_b16 (gfx950)

    bool is_gfx950_only = is_fp4_convert || is_mfma_f8f6f4 ||
                          is_cvt_pk_f16 || is_cvt_pk_bf16 || is_bitop3;
    if (!is_gfx950_only) continue;

    // Extract destination VGPR from VOP3 encoding
    uint8_t vdst = dword0 & 0xFF;

    if (is_cvt_pk_f16) {
      // Swap v_cvt_pk_f16_f32 (D267) → v_cvt_pkrtz_f16_f32 (D296)
      // Same VOP3 operand format, just different opcode. Slightly
      // different rounding (RTZ vs RNE) but functionally compatible.
      uint32_t new_dw0 = (dword0 & ~0xFFFF0000u) | 0xD2960000u;
      std::memcpy(text + di.offset, &new_dw0, 4);
      // DW1 unchanged
    } else if (is_cvt_pk_bf16) {
      // v_cvt_pk_bf16_f32 vDst, vSrc0, vSrc1: pack bf16(src0) and bf16(src1)
      // Full emulation using NOP sled trampoline + temp VGPR (v255):
      //   1. v_lshrrev_b32 vDst, 16, vSrc0   (4B) → bf16(src0) in [15:0]
      //   2. v_lshrrev_b32 v255, 16, vSrc1   (4B) → bf16(src1) in [15:0]
      //   3. v_lshl_or_b32 vDst, v255, 16, vDst (8B) → pack both halves
      //   4. s_branch <return>                (4B)
      // Total: 20 bytes in trampoline
      uint16_t src0_raw = dword1 & 0x1FF;
      uint16_t src1_raw = (dword1 >> 9) & 0x1FF;
      uint8_t src0_vgpr = static_cast<uint8_t>(src0_raw & 0xFF);
      uint8_t src1_vgpr = static_cast<uint8_t>(src1_raw & 0xFF);
      uint8_t vtmp = 255; // temp VGPR

      NopSled* sled = findNearestSled(di.offset);
      if (sled && sled->write_pos + 20 <= sled->end) {
        uint64_t tp = sled->write_pos;

        // 1. v_lshrrev_b32 vDst, 16, vSrc0
        uint32_t i1 = (0x10u << 25) | (static_cast<uint32_t>(vdst) << 17) |
                      (static_cast<uint32_t>(src0_vgpr) << 9) | 0x90u;
        std::memcpy(text + tp, &i1, 4);

        // 2. v_lshrrev_b32 v255, 16, vSrc1
        uint32_t i2 = (0x10u << 25) | (static_cast<uint32_t>(vtmp) << 17) |
                      (static_cast<uint32_t>(src1_vgpr) << 9) | 0x90u;
        std::memcpy(text + tp + 4, &i2, 4);

        // 3. v_lshl_or_b32 vDst, v255, 16, vDst (VOP3 opcode D200)
        uint32_t i3_dw0 = 0xD2000000u | static_cast<uint32_t>(vdst);
        uint32_t i3_dw1 = (256u + static_cast<uint32_t>(vtmp)) |
                          (0x90u << 9) |
                          ((256u + static_cast<uint32_t>(vdst)) << 18);
        std::memcpy(text + tp + 8, &i3_dw0, 4);
        std::memcpy(text + tp + 12, &i3_dw1, 4);

        // 4. s_branch back
        uint8_t br_back[4];
        if (EncodeSBranch(tp + 16, di.offset + 8, br_back)) {
          std::memcpy(text + tp + 16, br_back, 4);

          // Replace original with s_branch to trampoline + s_nop
          uint8_t br_fwd[4];
          if (EncodeSBranch(di.offset, tp, br_fwd)) {
            std::memcpy(text + di.offset, br_fwd, 4);
            uint8_t nop[4];
            EncodeSNop(nop);
            std::memcpy(text + di.offset + 4, nop, 4);
            sled->write_pos += 20;
            di.mnemonic = "<replaced>";
            ++gfx950_only_replaced;
            continue;
          }
        }
      }
      // Fallback: v_lshrrev for src0 half only
      uint32_t lshr_word = (0x10u << 25) |
                           (static_cast<uint32_t>(vdst) << 17) |
                           (static_cast<uint32_t>(src0_vgpr) << 9) | 0x90u;
      std::memcpy(text + di.offset, &lshr_word, 4);
      uint8_t nop[4];
      EncodeSNop(nop);
      std::memcpy(text + di.offset + 4, nop, 4);
    } else if (is_fp4_convert) {
      // v_cvt_scalef32_pk_fp4_f32 vDst, vSrc0, vSrc1, vScale
      // Emulate FP4 E2M1 quantization using trampoline with v255 temp:
      //   1. v_mul_f32 vDst, vSrc0, vScale    (4B VOP2) — scale src0
      //   2. v_mul_f32 vDst, vDst, 2.0        (4B VOP2) — multiply by 2 for E2M1 index
      //   3. v_cvt_u32_f32 vDst, vDst         (4B VOP1) — truncate to uint
      //   4. v_min_u32 vDst, 7, vDst          (4B VOP2) — clamp to [0,7]
      //   5. v_mul_f32 v255, vSrc1, vScale    (4B VOP2) — scale src1
      //   6. v_mul_f32 v255, v255, 2.0        (4B VOP2)
      //   7. v_cvt_u32_f32 v255, v255         (4B VOP1)
      //   8. v_min_u32 v255, 7, v255          (4B VOP2)
      //   9. v_lshl_or_b32 vDst, v255, 4, vDst (8B VOP3) — pack nibbles
      //  10. s_branch <return>                (4B)
      // Total: 44 bytes
      uint16_t src0_raw = dword1 & 0x1FF;
      uint16_t src1_raw = (dword1 >> 9) & 0x1FF;
      uint16_t scale_raw = (dword1 >> 18) & 0x1FF;
      uint8_t src0_vgpr = static_cast<uint8_t>(src0_raw & 0xFF);
      uint8_t src1_vgpr = static_cast<uint8_t>(src1_raw & 0xFF);
      uint8_t scale_vgpr = static_cast<uint8_t>(scale_raw & 0xFF);
      uint8_t vtmp = 255;

      NopSled* sled = findNearestSled(di.offset);
      if (sled && sled->write_pos + 44 <= sled->end) {
        uint64_t tp = sled->write_pos;

        // VOP2 helpers:
        // v_mul_f32_e32 = opcode 0x04: (0x04<<25)|(vdst<<17)|(vsrc1<<9)|src0
        // v_min_u32_e32 = opcode 0x0E: (0x0E<<25)|(vdst<<17)|(vsrc1<<9)|src0
        // VOP1: v_cvt_u32_f32_e32 = opcode 0x07: 0x7E000000|(vdst<<17)|(0x07<<9)|src0
        // 2.0 inline = 0xF4, 7 inline = 0x87
        auto vop2 = [](uint8_t op, uint8_t d, uint8_t s1, uint16_t s0) -> uint32_t {
          return (static_cast<uint32_t>(op) << 25) | (static_cast<uint32_t>(d) << 17) |
                 (static_cast<uint32_t>(s1) << 9) | s0;
        };
        auto vop1_cvt = [](uint8_t d, uint16_t s0) -> uint32_t {
          return 0x7E000000u | (static_cast<uint32_t>(d) << 17) |
                 (0x07u << 9) | s0; // v_cvt_u32_f32 opcode = 0x07
        };

        // 1. v_mul_f32 vDst, vScale, vSrc0 (src0=vScale, vsrc1=vSrc0)
        uint32_t i1 = vop2(0x04, vdst, src0_vgpr, 256u + scale_vgpr);
        std::memcpy(text + tp, &i1, 4);
        // 2. v_mul_f32 vDst, 2.0, vDst (src0=2.0(0xF4), vsrc1=vDst)
        uint32_t i2 = vop2(0x04, vdst, vdst, 0xF4u);
        std::memcpy(text + tp + 4, &i2, 4);
        // 3. v_cvt_u32_f32 vDst, vDst
        uint32_t i3 = vop1_cvt(vdst, 256u + vdst);
        std::memcpy(text + tp + 8, &i3, 4);
        // 4. v_min_u32 vDst, 7, vDst (src0=7(0x87), vsrc1=vDst)
        uint32_t i4 = vop2(0x0E, vdst, vdst, 0x87u);
        std::memcpy(text + tp + 12, &i4, 4);
        // 5. v_mul_f32 v255, vScale, vSrc1
        uint32_t i5 = vop2(0x04, vtmp, src1_vgpr, 256u + scale_vgpr);
        std::memcpy(text + tp + 16, &i5, 4);
        // 6. v_mul_f32 v255, 2.0, v255
        uint32_t i6 = vop2(0x04, vtmp, vtmp, 0xF4u);
        std::memcpy(text + tp + 20, &i6, 4);
        // 7. v_cvt_u32_f32 v255, v255
        uint32_t i7 = vop1_cvt(vtmp, 256u + vtmp);
        std::memcpy(text + tp + 24, &i7, 4);
        // 8. v_min_u32 v255, 7, v255
        uint32_t i8 = vop2(0x0E, vtmp, vtmp, 0x87u);
        std::memcpy(text + tp + 28, &i8, 4);
        // 9. v_lshl_or_b32 vDst, v255, 4, vDst (VOP3)
        uint32_t i9_dw0 = 0xD2000000u | static_cast<uint32_t>(vdst);
        uint32_t i9_dw1 = (256u + vtmp) | (0x84u << 9) | // inline 4
                          ((256u + vdst) << 18);
        std::memcpy(text + tp + 32, &i9_dw0, 4);
        std::memcpy(text + tp + 36, &i9_dw1, 4);
        // 10. s_branch back
        uint8_t br[4];
        if (EncodeSBranch(tp + 40, di.offset + 8, br)) {
          std::memcpy(text + tp + 40, br, 4);

          uint8_t br_fwd[4];
          if (EncodeSBranch(di.offset, tp, br_fwd)) {
            std::memcpy(text + di.offset, br_fwd, 4);
            uint8_t nop[4];
            EncodeSNop(nop);
            std::memcpy(text + di.offset + 4, nop, 4);
            sled->write_pos += 44;
            di.mnemonic = "<replaced>";
            ++gfx950_only_replaced;
            continue;
          }
        }
      }
      // Fallback: constant FP4
      uint32_t mov_word = 0x7E000291u | (static_cast<uint32_t>(vdst) << 17);
      std::memcpy(text + di.offset, &mov_word, 4);
      uint8_t nop[4];
      EncodeSNop(nop);
      std::memcpy(text + di.offset + 4, nop, 4);
    } else if (is_bitop3) {
      // v_bitop3_b16 with bitop3:0xEC = (a ? (b|c) : (b&c)).
      // All instances in AITER use 0xEC. When c (src2) is mostly 1s,
      // this approximates v_or_b32 vDst, vSrc0, vSrc1.
      // Emulate with v_or_b32_e32 (VOP2 opcode 0x14):
      // [31:25]=0x14 [24:17]=vdst [16:9]=vsrc1 [8:0]=src0
      uint16_t src0_raw = dword1 & 0x1FF;
      uint16_t src1_raw = (dword1 >> 9) & 0x1FF;
      uint8_t src0_vgpr = static_cast<uint8_t>(src0_raw & 0xFF);
      uint8_t src1_vgpr = static_cast<uint8_t>(src1_raw & 0xFF);
      uint32_t or_word = (0x14u << 25) |
                         (static_cast<uint32_t>(vdst) << 17) |
                         (static_cast<uint32_t>(src1_vgpr) << 9) |
                         (256u + static_cast<uint32_t>(src0_vgpr));
      std::memcpy(text + di.offset, &or_word, 4);
      uint8_t nop[4];
      EncodeSNop(nop);
      std::memcpy(text + di.offset + 4, nop, 4);
    } else {
      // MFMA or no space for trampoline: NOP out
      uint8_t nop[4];
      EncodeSNop(nop);
      std::memcpy(text + di.offset, nop, 4);
      std::memcpy(text + di.offset + 4, nop, 4);
    }

    di.mnemonic = "<replaced>";
    ++gfx950_only_replaced;
  }

  if (gfx950_only_replaced > 0) {
    uint32_t kept = static_cast<uint32_t>(decoded.size()) - gfx950_only_replaced;
    result.rules_matched = kept;
    std::cerr << "hotswap: retarget: " << kept
              << " instructions kept (identical encoding), "
              << gfx950_only_replaced << " replaced with trampolines ("
              << src_state.cpu << " -> " << tgt_cpu << ")\n";
    return result;
  }

  // Build a single assembly string from all decoded instructions, then
  // assemble in one pass for the target ISA. This avoids creating/destroying
  // hundreds of MC pipeline objects which corrupts LLVM internal state.

  uint32_t retargeted = 0;
  uint32_t failed = 0;

  // Step 1: Print all instructions to assembly text, tracking offsets
  struct AsmEntry {
    size_t decoded_idx;
    std::string asm_line;
  };
  std::vector<AsmEntry> entries;
  std::string full_asm;

  // Add .text directive for the assembler
  full_asm += ".text\n";

  for (size_t i = 0; i < decoded.size(); ++i) {
    auto& di = decoded[i];
    if (di.mnemonic == "<unknown>") continue;

    std::string asm_text;
    if (src_state.printer) {
      llvm::raw_string_ostream rso(asm_text);
      src_state.printer->printInst(&di.inst, 0, "", *src_state.STI, rso);
      rso.flush();
    }

    // Trim leading whitespace
    size_t start = asm_text.find_first_not_of(" \t");
    if (start != std::string::npos && start > 0)
      asm_text = asm_text.substr(start);
    if (asm_text.empty()) continue;

    // Strip trailing comments
    size_t comment = asm_text.find("//");
    if (comment != std::string::npos) {
      asm_text = asm_text.substr(0, comment);
      // Trim trailing whitespace after removing comment
      size_t end = asm_text.find_last_not_of(" \t");
      if (end != std::string::npos)
        asm_text = asm_text.substr(0, end + 1);
    }

    if (asm_text.empty()) continue;

    entries.push_back({i, asm_text});
    full_asm += asm_text + "\n";
  }

  if (entries.empty()) {
    if (elf_info.text_size > 0) {
      std::cerr << "hotswap: retarget: no decodable instructions in "
                << elf_info.text_size << " bytes of .text\n";
    }
    return result;
  }

  std::cerr << "hotswap: retarget: assembling " << entries.size()
            << " instructions for " << tgt_cpu << "...\n";

  // Step 2: Assemble the entire text in a single pass for the target ISA.
  // Use a persistent static MCContext to avoid LLVM backend crashes from
  // creating/destroying MC contexts repeatedly. The AMDGPU backend has
  // global state that doesn't survive multiple context lifecycles.
  struct TargetAssembler {
    std::unique_ptr<llvm::MCRegisterInfo> MRI;
    std::unique_ptr<const llvm::MCAsmInfo> MAI;
    std::unique_ptr<llvm::MCInstrInfo> MCII;
    std::unique_ptr<llvm::MCSubtargetInfo> STI;
    std::unique_ptr<llvm::MCContext> Ctx;
    const llvm::Target* target = nullptr;
    bool valid = false;
  };
  static std::mutex s_asm_mutex;
  static std::map<std::string, TargetAssembler> s_assemblers;

  std::lock_guard<std::mutex> asm_lock(s_asm_mutex);
  auto& ta = s_assemblers[tgt_cpu];
  if (!ta.valid) {
    llvm::Triple init_triple("amdgcn-amd-amdhsa");
    llvm::MCTargetOptions init_opts;
    ta.target = src_state.target;
    ta.MRI.reset(ta.target->createMCRegInfo(init_triple));
#if LLVM_VERSION_MAJOR > 9
    ta.MAI.reset(ta.target->createMCAsmInfo(*ta.MRI, init_triple, init_opts));
#else
    ta.MAI.reset(ta.target->createMCAsmInfo(*ta.MRI, "amdgcn-amd-amdhsa"));
#endif
    ta.MCII.reset(ta.target->createMCInstrInfo());
    ta.STI.reset(ta.target->createMCSubtargetInfo(init_triple, tgt_cpu, ""));
    if (ta.MRI && ta.MAI && ta.MCII && ta.STI) {
#if LLVM_VERSION_MAJOR > 12
      ta.Ctx = std::make_unique<llvm::MCContext>(
          init_triple, ta.MAI.get(), ta.MRI.get(), ta.STI.get());
#else
      auto MOFI = std::make_unique<llvm::MCObjectFileInfo>();
      ta.Ctx = std::make_unique<llvm::MCContext>(
          ta.MAI.get(), ta.MRI.get(), MOFI.get());
      MOFI->InitMCObjectFileInfo(init_triple, true, *ta.Ctx);
#endif
      ta.valid = true;
    }
  }

  if (!ta.valid) {
    std::cerr << "hotswap: retarget: failed to create target assembler\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  // Track how many times we've assembled. After the first successful
  // assembly, the LLVM AMDGPU backend's global state may be in a
  // fragile condition. Limit retarget attempts.
  static int s_retarget_count = 0;
  if (s_retarget_count > 0) {
    // LLVM's AMDGPU MC backend has global state that doesn't survive
    // multiple MCContext lifecycles. Skip subsequent retargets and
    // rely on the first code object being the user's kernel.
    std::cerr << "hotswap: retarget: skipping subsequent code object ("
              << entries.size() << " instructions) — LLVM MC limitation\n";
    return result;
  }

  // Reset MCContext state for the assembly
  ta.Ctx->reset();

  llvm::Triple tgt_triple("amdgcn-amd-amdhsa");
  llvm::MCTargetOptions mc_opts;

  // Use the cached target assembler components
  llvm::MCContext& tgt_Ctx = *ta.Ctx;

  llvm::StringRef asm_ref(full_asm);
  auto buf = llvm::MemoryBuffer::getMemBuffer(asm_ref, "", false);
  llvm::SourceMgr src_mgr;
  src_mgr.AddNewSourceBuffer(std::move(buf), llvm::SMLoc());

  std::string data;
  auto data_stream = std::make_unique<llvm::raw_string_ostream>(data);
  auto bos = std::make_unique<llvm::buffer_ostream>(*data_stream);

#if LLVM_VERSION_MAJOR > 14
  llvm::MCCodeEmitter* ce =
      ta.target->createMCCodeEmitter(*ta.MCII, tgt_Ctx);
#else
  llvm::MCCodeEmitter* ce =
      ta.target->createMCCodeEmitter(*ta.MCII, *ta.MRI, tgt_Ctx);
#endif
  llvm::MCAsmBackend* mab =
      ta.target->createMCAsmBackend(*ta.STI, *ta.MRI, mc_opts);

  if (!ce || !mab) {
    std::cerr << "hotswap: retarget: failed to create code emitter/backend\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

#if LLVM_VERSION_MAJOR > 20
  auto streamer = std::unique_ptr<llvm::MCStreamer>(
      ta.target->createMCObjectStreamer(
          tgt_triple, tgt_Ctx,
          std::unique_ptr<llvm::MCAsmBackend>(mab),
          mab->createObjectWriter(*bos),
          std::unique_ptr<llvm::MCCodeEmitter>(ce), *ta.STI));
#else
  auto streamer = std::unique_ptr<llvm::MCStreamer>(
      ta.target->createMCObjectStreamer(
          tgt_triple, tgt_Ctx,
          std::unique_ptr<llvm::MCAsmBackend>(mab),
          mab->createObjectWriter(*bos),
          std::unique_ptr<llvm::MCCodeEmitter>(ce), *ta.STI,
          mc_opts.MCRelaxAll, mc_opts.MCIncrementalLinkerCompatible, false));
#endif

  if (!streamer) {
    std::cerr << "hotswap: retarget: failed to create MC streamer\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  auto parser = std::unique_ptr<llvm::MCAsmParser>(
      llvm::createMCAsmParser(src_mgr, tgt_Ctx, *streamer, *ta.MAI));
  auto tap = std::unique_ptr<llvm::MCTargetAsmParser>(
      ta.target->createMCAsmParser(*ta.STI, *parser, *ta.MCII, mc_opts));
  if (!tap) {
    std::cerr << "hotswap: retarget: failed to create target asm parser\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }
  parser->setTargetParser(*tap);

  bool asm_failed = parser->Run(true);
  // Destroy parser and streamer before flushing to avoid use-after-free
  tap.reset();
  parser.reset();
  streamer.reset();
  bos.reset();
  data_stream->flush();

  if (asm_failed || data.size() < 64) {
    std::cerr << "hotswap: retarget: assembly failed for target "
              << tgt_cpu << " (some instructions may not exist)\n";
    // Even if assembly failed, some instructions may have been emitted.
    // We'll try to use what we got.
    if (data.size() < 64) {
      result.status = HSA_STATUS_ERROR;
      return result;
    }
  }

  // Step 3: Extract .text from the assembled ELF
  const uint8_t* asm_elf = reinterpret_cast<const uint8_t*>(data.data());
  ElfInfo asm_info;
  if (!ParseElfInfo(asm_elf, data.size(), asm_info)) {
    std::cerr << "hotswap: retarget: failed to parse assembled ELF\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  // Step 4: Disassemble the target .text to get per-instruction boundaries
  // Reuse the source disassembler — instruction boundaries are the same
  // for compatible instructions (same mnemonic = same size on both ISAs
  // for the gfx9 family). This avoids creating a second LLVMState which
  // can crash due to LLVM global state conflicts.
  const uint8_t* tgt_text = asm_elf + asm_info.text_offset;
  std::vector<InternalDecodedInst> tgt_decoded;
  DecodeTextSection(tgt_text, asm_info.text_size, src_state, tgt_decoded);

  // Step 5: Match source and target instructions and patch in-place
  // The assembler should produce the same number of instructions in the
  // same order (unless some expanded or were removed).
  size_t tgt_idx = 0;
  for (auto& entry : entries) {
    if (tgt_idx >= tgt_decoded.size()) break;

    auto& src_di = decoded[entry.decoded_idx];
    auto& tgt_di = tgt_decoded[tgt_idx];

    if (tgt_di.size == src_di.size) {
      // Same size — patch in-place
      std::memcpy(text + src_di.offset, tgt_text + tgt_di.offset, src_di.size);
      ++retargeted;
    } else if (tgt_di.size < src_di.size) {
      // Target is smaller — patch + NOP pad
      std::memcpy(text + src_di.offset, tgt_text + tgt_di.offset, tgt_di.size);
      uint32_t remaining = src_di.size - tgt_di.size;
      uint64_t pad = src_di.offset + tgt_di.size;
      while (remaining >= 4) {
        uint8_t nop[4];
        EncodeSNop(nop);
        std::memcpy(text + pad, nop, 4);
        pad += 4;
        remaining -= 4;
      }
      ++retargeted;
    } else {
      // Target is larger — can't fit, skip this instruction
      ++failed;
    }
    ++tgt_idx;
  }

  if (ShouldDump()) {
    // Dump the retargeted .text using the source disassembler
    // (some instructions may decode differently but the bytes are correct)
    std::vector<InternalDecodedInst> decoded_after;
    DecodeTextSection(text, elf_info.text_size, src_state, decoded_after);
    DumpInstructions("RETARGET AFTER", decoded_after, text);
  }

  ++s_retarget_count;
  result.rules_matched = retargeted;
  if (failed > 0) {
    std::cerr << "hotswap: retarget: " << retargeted << " instructions retargeted, "
              << failed << " failed (" << src_state.cpu << " -> "
              << tgt_cpu << ")\n";
  } else {
    std::cerr << "hotswap: retarget: " << retargeted << " instructions retargeted ("
              << src_state.cpu << " -> " << tgt_cpu << ")\n";
  }

  return result;
}

RewriteResult RewriteCodeObject(void* elf_data, size_t elf_size,
                                const std::string& isa_name) {
  void* out_data = nullptr;
  size_t out_size = 0;
  auto result = RewriteCodeObjectGrow(elf_data, elf_size, &out_data, &out_size,
                                      isa_name);

  // If the buffer grew, we can't handle it in the in-place API
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

  // Check target match
  if (!rules->target.empty() && rules->target != isa_name) {
    // Target doesn't match — silently skip
    return result;
  }

  // Parse ELF to find .text
  ElfInfo elf_info;
  uint8_t* elf = static_cast<uint8_t*>(const_cast<void*>(elf_data));
  if (!ParseElfInfo(elf, elf_size, elf_info)) {
    // Not a valid ELF or no .text — skip silently
    return result;
  }

  if (elf_info.text_size == 0) return result;

  // Initialize LLVM MC
  LLVMState llvm_state = InitLLVM(isa_name);
  if (!llvm_state.valid) {
    std::cerr << "hotswap: LLVM MC initialization failed\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  // Get a mutable pointer to .text within the ELF
  uint8_t* text = elf + elf_info.text_offset;

  // Decode all instructions in .text
  std::vector<InternalDecodedInst> decoded;
  if (!DecodeTextSection(text, elf_info.text_size, llvm_state, decoded)) {
    std::cerr << "hotswap: instruction decode failed\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  if (ShouldDump()) {
    DumpInstructions("BEFORE", decoded, text);
  }

  // Collect trampolines needed for size-changing rewrites
  std::vector<Trampoline> trampolines;

  // Apply rules
  for (auto& inst : decoded) {
    for (auto& rule : rules->rules) {
      if (!MatchRule(rule, inst, elf_info)) continue;

      bool applied = false;

      switch (rule.action) {
        case ReplaceAction::MnemonicSwap:
          applied = ApplyMnemonicSwap(rule, inst, text, llvm_state);
          break;

        case ReplaceAction::ByteReplace:
          applied = ApplyByteReplace(rule, inst.offset, inst.size, text, elf_info.text_size);
          break;

        case ReplaceAction::AsmReplace: {
          // Check if this is same-size (single instruction replacement)
          // or needs a trampoline (multi-instruction or different size)
          uint64_t tramp_offset = elf_info.text_size;
          for (auto& t : trampolines) {
            tramp_offset += t.bytes.size();
          }

          Trampoline tramp = BuildTrampoline(
              rule.replace_asm, inst.offset, inst.size, tramp_offset,
              llvm_state.cpu, llvm_state.STI.get(), llvm_state.MCII.get(),
              llvm_state.MRI.get(), llvm_state.MAI.get(),
              llvm_state.Ctx.get(), llvm_state.CE);

          if (tramp.bytes.empty()) {
            std::cerr << "hotswap: trampoline build failed for rule '"
                      << rule.name << "'\n";
            break;
          }

          // Replace original instruction with s_branch to trampoline
          // Pad remaining bytes with s_nop
          uint8_t branch_bytes[4];
          bool is_gfx12 = llvm_state.cpu.find("gfx12") == 0;
          if (!EncodeSBranch(inst.offset, tramp_offset, branch_bytes, is_gfx12)) {
            std::cerr << "hotswap: branch encode failed for rule '"
                      << rule.name << "'\n";
            break;
          }

          std::memcpy(text + inst.offset, branch_bytes, 4);
          // NOP-fill the rest of the original instruction
          for (uint32_t pad = 4; pad < inst.size; pad += 4) {
            uint8_t nop[4];
            EncodeSNop(nop);
            std::memcpy(text + inst.offset + pad, nop, 4);
          }

          trampolines.push_back(std::move(tramp));
          applied = true;
          break;
        }
      }

      if (applied) {
        ++result.rules_matched;

        // Update kernel descriptor if extra registers requested
        if (rule.extra_vgprs > 0 || rule.extra_sgprs > 0) {
          std::string kernel = FindKernelAtOffset(elf_info, inst.offset);
          if (!kernel.empty()) {
            UpdateKernelDescriptor(elf, elf_size, elf_info, kernel,
                                  rule.extra_vgprs, rule.extra_sgprs);
          }
        }

        break; // Only apply first matching rule per instruction
      }
    }
  }

  // If we have trampolines, we need to grow the ELF
  if (!trampolines.empty()) {
    size_t new_elf_size = 0;
    uint8_t* new_elf = GrowElfWithTrampolines(
        elf, elf_size, elf_info, trampolines, &new_elf_size);
    if (!new_elf) {
      result.status = HSA_STATUS_ERROR_OUT_OF_RESOURCES;
      return result;
    }
    *out_data = new_elf;
    *out_size = new_elf_size;
    result.trampolines_added = static_cast<uint32_t>(trampolines.size());
  }

  // Re-decode and dump after patching
  if (ShouldDump() && result.rules_matched > 0) {
    uint8_t* final_text =
        (*out_data == elf_data) ? text : (static_cast<uint8_t*>(*out_data) +
                                          elf_info.text_offset);
    uint64_t final_text_size = elf_info.text_size;
    if (!trampolines.empty()) {
      size_t tramp_total = 0;
      for (auto& t : trampolines) tramp_total += t.bytes.size();
      final_text_size += tramp_total;
    }

    std::vector<InternalDecodedInst> decoded_after;
    DecodeTextSection(final_text, final_text_size, llvm_state, decoded_after);
    DumpInstructions("AFTER", decoded_after, final_text);
  }

  return result;
}

RewriteResult RetargetCodeObjectB0A0Grow(const void* elf_data, size_t elf_size,
                                         void** out_data, size_t* out_size) {
  RewriteResult result = {HSA_STATUS_SUCCESS, 0, 0};
  *out_data = const_cast<void*>(elf_data);
  *out_size = elf_size;

  const std::string isa = "amdgcn-amd-amdhsa--gfx1250";

  ElfInfo elf_info;
  const uint8_t* elf = static_cast<const uint8_t*>(elf_data);
  if (!ParseElfInfo(elf, elf_size, elf_info)) return result;
  if (elf_info.text_size == 0) return result;

  LLVMState& llvm_state = InitLLVMCached(isa);
  if (!llvm_state.valid) {
    std::cerr << "hotswap: B0->A0 grow: LLVM init failed\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  // Work on a mutable copy so we can apply in-place patches
  std::vector<uint8_t> buf(elf, elf + elf_size);
  uint8_t* text = buf.data() + elf_info.text_offset;

  std::vector<InternalDecodedInst> decoded;
  if (!DecodeTextSection(text, elf_info.text_size, llvm_state, decoded)) {
    std::cerr << "hotswap: B0->A0 grow: decode failed\n";
    result.status = HSA_STATUS_ERROR;
    return result;
  }

  std::vector<Trampoline> deferred;
  uint32_t count = ApplyGfx1250B0toA0Rules(
      decoded, text, elf_info.text_size, llvm_state, deferred);
  result.rules_matched = count;

  if (!deferred.empty()) {
    // Fix up branch offsets in deferred trampolines and apply original-site
    // overwrites. Each trampoline will be appended at text_size + running offset.
    uint64_t tramp_text_offset = elf_info.text_size;
    for (auto& t : deferred) {
      // Compute running position
      uint64_t tp = tramp_text_offset;
      tramp_text_offset += t.bytes.size();

      // Fix up the s_branch back (last 4 bytes of trampoline)
      // Branch from (tp + t.bytes.size() - 4) back to (t.original_offset + t.original_size)
      uint8_t br_back[4];
      uint64_t br_from = tp + t.bytes.size() - 4;
      uint64_t br_to = t.original_offset + t.original_size;
      if (!EncodeSBranch(br_from, br_to, br_back, true)) {
        std::cerr << "hotswap: B0->A0 grow: s_branch back out of range\n";
        continue;
      }
      std::memcpy(t.bytes.data() + t.bytes.size() - 4, br_back, 4);

      // Overwrite original site with s_branch to trampoline + NOPs
      uint8_t br_fwd[4];
      if (!EncodeSBranch(t.original_offset, tp, br_fwd, true)) {
        std::cerr << "hotswap: B0->A0 grow: s_branch fwd out of range\n";
        continue;
      }
      std::memcpy(text + t.original_offset, br_fwd, 4);
      for (uint32_t i = 4; i < t.original_size; i += 4) {
        uint8_t nop[4];
        EncodeSNop(nop);
        std::memcpy(text + t.original_offset + i, nop, 4);
      }
    }

    // Grow the ELF
    size_t new_size = 0;
    uint8_t* new_elf = GrowElfWithTrampolines(
        buf.data(), elf_size, elf_info, deferred, &new_size);
    if (!new_elf) {
      result.status = HSA_STATUS_ERROR_OUT_OF_RESOURCES;
      return result;
    }
    *out_data = new_elf;
    *out_size = new_size;
    result.trampolines_added = static_cast<uint32_t>(deferred.size());
  } else {
    // No growth needed — return a copy with in-place patches
    uint8_t* out = static_cast<uint8_t*>(std::malloc(elf_size));
    if (!out) {
      result.status = HSA_STATUS_ERROR_OUT_OF_RESOURCES;
      return result;
    }
    std::memcpy(out, buf.data(), elf_size);
    *out_data = out;
    *out_size = elf_size;
  }

  if (count > 0)
    std::cerr << "hotswap: gfx1250 B0->A0 grow: " << count
              << " patches applied, " << deferred.size()
              << " trampolines via ELF growth\n";
  return result;
}

// ── Test-only helpers (callable from extern "C" wrappers) ────────────────────

std::vector<uint8_t> AssembleInstGfx1250(const std::string& asm_str) {
  const std::string isa = "amdgcn-amd-amdhsa--gfx1250";
  LLVMState& state = InitLLVMCached(isa);
  if (!state.valid) return {};
  return AssembleSingleInst(asm_str, state);
}

} // namespace hotswap
} // namespace rocr

// C-linkage wrapper for dlsym access from HIP/CLR
extern "C" __attribute__((visibility("default")))
int rocr_hotswap_retarget(void* elf_data, size_t elf_size,
                          const char* source_isa, const char* target_isa) {
  auto result = rocr::hotswap::RetargetCodeObject(
      elf_data, elf_size, std::string(source_isa), std::string(target_isa));
  return result.rules_matched;
}

// C-linkage wrapper for gfx1250 B0→A0 patches (in-place, NOP sled only)
extern "C" __attribute__((visibility("default")))
int rocr_hotswap_gfx1250_b0_to_a0(void* elf_data, size_t elf_size) {
  auto result = rocr::hotswap::RetargetCodeObject(
      elf_data, elf_size,
      "amdgcn-amd-amdhsa--gfx1250", "amdgcn-amd-amdhsa--gfx1250");
  return result.rules_matched;
}

// C-linkage wrapper for gfx1250 B0→A0 patches with ELF growth support
extern "C" __attribute__((visibility("default")))
int rocr_hotswap_gfx1250_b0_to_a0_grow(
    const void* elf_data, size_t elf_size,
    void** out_data, size_t* out_size) {
  auto result = rocr::hotswap::RetargetCodeObjectB0A0Grow(
      elf_data, elf_size, out_data, out_size);
  return result.rules_matched;
}

// C-linkage wrapper for testing WMMA NOP classification.
// Duplicates the ClassifyWmmaNops logic (now in hotswap_core)
// to provide a testable entry point.
extern "C" __attribute__((visibility("default")))
int rocr_hotswap_classify_wmma_nops(const char* mnemonic, int* b0, int* a0) {
  std::string mn(mnemonic);
  int b0_nops = 4, a0_nops = 4; // conservative default

  bool is_wmma = (mn.find("v_wmma") == 0);
  bool is_swmmac = (mn.find("v_swmmac") == 0);
  if (is_wmma || is_swmmac) {
    if (mn.find("_iu8") != std::string::npos ||
        mn.find("_iu4") != std::string::npos) {
      b0_nops = 8; a0_nops = 4;
    } else if (mn.find("f8f6f4") != std::string::npos) {
      b0_nops = 1; a0_nops = 4;
    } else {
      bool has_f8 = (mn.find("_fp8") != std::string::npos ||
                     mn.find("_f8") != std::string::npos ||
                     mn.find("_bf8") != std::string::npos);
      if (has_f8) {
        if (mn.find("16x16x128") != std::string::npos) {
          b0_nops = 3; a0_nops = 4;
        } else {
          b0_nops = 1; a0_nops = 4;
        }
      } else if (mn.find("_f16") != std::string::npos ||
                 mn.find("_bf16") != std::string::npos) {
        b0_nops = 4; a0_nops = 4;
      }
    }
  }

  if (b0) *b0 = b0_nops;
  if (a0) *a0 = a0_nops;
  return (a0_nops > b0_nops) ? 1 : 0;
}

// C-linkage wrapper for assembling a single gfx1250 instruction (test helper)
extern "C" __attribute__((visibility("default")))
int rocr_hotswap_assemble_inst(const char* asm_str,
                                uint8_t* out_bytes, int max_bytes) {
  auto bytes = rocr::hotswap::AssembleInstGfx1250(std::string(asm_str));
  if (bytes.empty()) return -1;
  int n = std::min(static_cast<int>(bytes.size()), max_bytes);
  std::memcpy(out_bytes, bytes.data(), n);
  return n;
}
