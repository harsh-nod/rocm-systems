////////////////////////////////////////////////////////////////////////////////
//
// Internal header for LLVM-dependent hotswap declarations.
// This header is NOT part of the public hotswap API and requires LLVM headers.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef ROCR_HOTSWAP_LLVM_INTERNAL_HPP
#define ROCR_HOTSWAP_LLVM_INTERNAL_HPP

#include "trampoline.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class MCSubtargetInfo;
class MCInstrInfo;
class MCRegisterInfo;
class MCAsmInfo;
class MCContext;
class MCCodeEmitter;
} // namespace llvm

namespace rocr {
namespace hotswap {

Trampoline BuildTrampoline(const std::vector<std::string>& asm_lines,
                           uint64_t original_offset,
                           uint32_t original_size,
                           uint64_t trampoline_text_offset,
                           const std::string& cpu,
                           llvm::MCSubtargetInfo* STI,
                           llvm::MCInstrInfo* MCII,
                           llvm::MCRegisterInfo* MRI,
                           const llvm::MCAsmInfo* MAI,
                           llvm::MCContext* Ctx,
                           llvm::MCCodeEmitter* CE);

} // namespace hotswap
} // namespace rocr

#endif // ROCR_HOTSWAP_LLVM_INTERNAL_HPP
