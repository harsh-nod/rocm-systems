# ROCm HotSwap: Implementation Guide for Contributors

> **Purpose:** This document is a comprehensive reference for agents and developers
> working on the hotswap feature in `rocm-systems`. It synthesizes the design
> documentation, commit history, and source code into a single guide covering
> high-level architecture, code organization, data flows, and implementation
> details.

## Table of Contents

1. [What HotSwap Is](#1-what-hotswap-is)
2. [High-Level Architecture](#2-high-level-architecture)
3. [Operating Modes](#3-operating-modes)
4. [Code Organization](#4-code-organization)
5. [Environment Variables](#5-environment-variables)
6. [Integration Points](#6-integration-points)
7. [Core Implementation Details](#7-core-implementation-details)
8. [COMGR Backend (Instruction Rewriting)](#8-comgr-backend-instruction-rewriting)
9. [Cross-Family Transpiler (gfx1250 → gfx9)](#9-cross-family-transpiler-gfx1250--gfx9)
10. [gfx1250 B0→A0 Patching](#10-gfx1250-b0a0-patching)
11. [Test Infrastructure](#11-test-infrastructure)
12. [Build System](#12-build-system)
13. [Key Data Structures](#13-key-data-structures)
14. [Execution Flow Walkthrough](#14-execution-flow-walkthrough)
15. [Known Limitations and Gaps](#15-known-limitations-and-gaps)
16. [Glossary](#16-glossary)

---

## 1. What HotSwap Is

HotSwap is a **load-time ISA rewriting system** embedded in the ROCR (HSA) runtime.
It intercepts GPU code object loading and transparently rewrites instructions so
that binaries compiled for one AMD GPU ISA can execute on a different GPU.

Applications require **no source changes, recompilation, or relinking**. HotSwap
operates at the ELF binary level, modifying the `.text` section, kernel
descriptors, and ELF metadata before the code reaches GPU memory.

### Use Cases


| Use Case                   | Example                                                                                           |
| -------------------------- | ------------------------------------------------------------------------------------------------- |
| **Same-family retarget**   | Run gfx950 (MI355X) binaries on gfx942 (MI300X) — 98.4% of instructions share identical encodings |
| **Cross-family transpile** | Run gfx1250 (RDNA4) binaries on gfx942/gfx950 (CDNA) — full disassemble→translate→reassemble      |
| **Stepping compatibility** | gfx1250 B0→A0 patching for hardware revision differences                                          |
| **Instruction tuning**     | JSON rule files to swap individual instructions without rebuilding kernels                        |
| **Instrumentation**        | Insert trampoline-based probes for profiling or debugging                                         |


---

## 2. High-Level Architecture

```
 ┌──────────────────────────────────────────────────────────────────┐
 │                        HIP Application                          │
 └──────────────────────┬───────────────────────────────────────────┘
                        │
                        ▼
 ┌──────────────────────────────────────────────────────────────────┐
 │  HIP Fat Binary Loader (hip_fatbin.cpp)                         │
 │                                                                  │
 │  • COMGR extracts code object from fat binary                   │
 │  • If no native CO: query cross-gen ISA (hotswap_extra_isas)    │
 │  • Patch ELF e_flags + .note ISA strings                        │
 │  • Optional: dlsym("rocr_hotswap_retarget") for early retarget  │
 └──────────────────────┬───────────────────────────────────────────┘
                        │
                        ▼
 ┌──────────────────────────────────────────────────────────────────┐
 │  ROCR Loader: ExecutableImpl::LoadCodeObject (executable.cpp)   │
 │                                                                  │
 │  ┌──────────────────────────────────────────────────────────┐   │
 │  │  HotSwap Pipeline (if ROCR_HOTSWAP_ENABLED && IsEnabled) │   │
 │  │                                                          │   │
 │  │  1. Detect ISA mismatch / override                       │   │
 │  │  2. Choose path:                                         │   │
 │  │     a. NeedsTranspile → TranspileCodeObject              │   │
 │  │     b. gfx1250 B0→A0  → RetargetCodeObjectB0A0Grow      │   │
 │  │     c. Same-family     → RetargetCodeObject + PatchElfIsa│   │
 │  │  3. Apply JSON rules   → RewriteCodeObject               │   │
 │  │  4. Re-init AmdHsaCode from rewritten buffer             │   │
 │  └──────────────────────────────────────────────────────────┘   │
 │                                                                  │
 │  → LoadSegments (copies patched code to GPU memory)             │
 └──────────────────────┬───────────────────────────────────────────┘
                        │
                        ▼
 ┌──────────────────────────────────────────────────────────────────┐
 │              GPU executes retargeted/transpiled code             │
 └──────────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **Separation of concerns:** ROCR's `hotswap/` directory is a **policy + glue +
  ELF metadata** layer. Instruction-level rewriting (retarget, transpile, rules)
   is delegated to **AMD COMGR** via `dlopen`/`dlsym` at runtime.
2. **No compile-time LLVM dependency:** The ROCR hotswap code is LLVM-free. COMGR
  (which embeds LLVM MC) is loaded dynamically. This keeps `libhsa-runtime64.so`
   lightweight.
3. **Environment-gated:** HotSwap is completely dormant unless `HSA_HOTSWAP_RULES`
  or `HSA_HOTSWAP_ISA_OVERRIDE` environment variables are set.
4. **Idempotent:** Applying the same transformation twice produces identical output.
  Tests verify this property.

---

## 3. Operating Modes

### Mode 1: Same-Family Retarget (gfx950 → gfx942)

**Key insight:** gfx942 and gfx950 share **identical binary encodings** for all
standard VALU/SMEM/VMEM/SOPP instructions. Only ~1.6% of instructions are
gfx950-specific and need patching.

- gfx950-only opcodes (`D23D`-`D243` FP4/FP6/FP8 scale converts, `D3AD`/`D3AE`
mixed-format MFMA) are replaced with NOPs or emulation trampolines
- ELF metadata (`e_flags`, `.note` ISA string) patched for gfx942
- Performance: **1.000x geomean** (zero overhead) across 20 test kernels
- Accuracy: **bit-identical** for standard instructions; approximate for FP4 emulation

### Mode 2: Cross-Family Transpile (gfx1250 → gfx942/gfx950)

Full disassemble→translate→reassemble pipeline (~3,200 lines). Handles:

- **Wave size adaptation:** Wave32 → Wave64 (half-wave execution with EXEC masking)
- **Opcode remapping:** GFX12 → GFX9 opcode tables
- **Encoding re-encoding:** SMEM, FLAT/GLOBAL, DS, VOP3 format differences
- **Wait counter merging:** `s_wait_loadcnt`/`s_wait_storecnt`/etc → `s_waitcnt`
- **SALU float emulation:** Scalar float → VALU promotion with `v_readfirstlane`
- **VOPD splitting:** Dual-issue → two separate VOP instructions
- **WMMA → MFMA:** Matrix instruction substitution with lane redistribution
- **Kernel descriptor translation:** VGPR/SGPR granularity, wave size bits, scratch model

Status: 18/20 test kernels passing (scalar, matmul, softmax, attention).

### Mode 3: gfx1250 B0→A0 Patching

In-ISA binary fixes for gfx1250 hardware stepping differences:

- `s_clause` patching
- Cluster load instruction rewrites
- `ds_store_2addr` → single-address DS ops
- `tensor_load_to_lds` patches (may require trampoline growth)
- WMMA scheduling hazard NOP insertion (B0 vs A0 cycle budgets)

### Mode 4: JSON Rule-Based Rewriting

User-supplied rules for targeted instruction replacement:

- `**mnemonic_swap`**: Change mnemonic, keep operands
- `**asm**`: Full assembly replacement (may trigger trampoline)
- `**bytes**`: Raw hex byte replacement

---

## 4. Code Organization

```
projects/rocr-runtime/runtime/hsa-runtime/
├── loader/
│   └── executable.cpp              # ROCR loader hook (integration point)
├── inc/
│   └── amd_hsa_elf.h              # EF_AMDGPU_MACH constants
├── core/runtime/
│   └── isa.cpp                    # ISA registry (gfx1250 entry)
├── hotswap/
│   ├── hotswap.hpp                # Public C++/C API
│   ├── hotswap.cpp                # Orchestration: PatchElfIsa, Retarget,
│   │                              #   Rewrite, B0A0, C exports
│   ├── hotswap_core.hpp           # ELF parsing, NOP sleds, trampolines,
│   ├── hotswap_core.cpp           #   WMMA hazards, mnemonic tables
│   ├── hotswap_rules.hpp          # JSON rule schema and parser
│   ├── hotswap_rules.cpp          # Zero-dependency JSON parser
│   ├── hotswap_comgr_client.hpp   # Dynamic COMGR binding interface
│   ├── hotswap_comgr_client.cpp   # dlopen/dlsym to amd_comgr_hotswap_*
│   ├── hotswap_shared_types.h     # ElfInfo, ElfSection, ElfSymbol
│   ├── transpiler.hpp             # Cross-family transpile API + stats
│   ├── trampoline.hpp             # s_branch / s_nop encoding helpers
│   ├── trampoline.cpp             # EncodeSBranch, EncodeSNop
│   ├── CMakeLists.txt             # Standalone test build
│   ├── docs/
│   │   ├── hotswap-architecture.md
│   │   ├── hotswap-rewrite-rules.md
│   │   ├── hotswap-wheel-integration.md
│   │   └── gfx1250-on-gfx950-analysis.md
│   └── tests/
│       ├── hotswap_test.cpp       # Unit tests (rules, trampolines, B0A0 minimal ELF)
│       ├── test_b0a0_hsaco.cpp    # B0A0 on real Triton .hsaco files
│       ├── test_b0a0_retarget.cpp # CLI tool for B0A0 retargeting
│       ├── test_dataflow.cpp      # COMGR dataflow analysis tests
│       ├── test_dwarf.cpp         # DWARF debug info preservation tests
│       ├── test_transpiler_comgr.cpp  # COMGR transpiler integration tests
│       ├── test_transpiler.py     # Assembly validity (llvm-mc) tests
│       ├── test_transpiler_e2e.py # End-to-end transpile pipeline tests
│       ├── test_hotswap_triton.py # Triton integration tests
│       ├── fuzz_hotswap.cpp       # Fuzz harness (AFL/libFuzzer)
│       ├── test_suite.hip         # GPU execution validation (20 kernels)
│       ├── test_multi_new.hip     # Subset GPU validation (7 kernels)
│       ├── hsaco_data/            # Precompiled Triton .hsaco test files
│       └── *.hip                  # Individual HIP test kernels
│
projects/clr/hipamd/src/
└── hip_fatbin.cpp                 # HIP fat binary cross-gen intercept
```

### File Responsibility Summary


| File                                | Responsibility                                                                                                                                                  |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `hotswap.hpp` / `hotswap.cpp`       | Public API, ISA enablement checks, `PatchElfIsa`, orchestration of COMGR calls, C exports (`rocr_hotswap_retarget`, `rocr_hotswap_gfx1250_b0_to_a0_grow`)       |
| `hotswap_core.hpp` / `.cpp`         | LLVM-free ELF surgery: `ParseElfInfo`, `ApplyByteReplace`, `UpdateKernelDescriptor`, `GrowElfWithTrampolines`, WMMA hazard classification, mnemonic swap tables |
| `hotswap_rules.hpp` / `.cpp`        | JSON rule parsing (`ParseRulesFile`, `ParseRulesString`, `GetCachedRules`), rule data structures (`RewriteRule`, `OperandMatch`, `ReplaceAction`)               |
| `hotswap_comgr_client.hpp` / `.cpp` | Dynamic binding to COMGR's `amd_comgr_hotswap_rewrite` and `amd_comgr_hotswap_needs_transpile` via `dlopen`                                                     |
| `transpiler.hpp`                    | Cross-family API surface (`NeedsTranspile`, `TranspileCodeObject`, `TranspileStats`)                                                                            |
| `trampoline.hpp` / `.cpp`           | Low-level `s_branch` and `s_nop` instruction encoding for GFX9 and GFX12                                                                                        |
| `hotswap_shared_types.h`            | Canonical ELF view types (`ElfInfo`, `ElfSection`, `ElfSymbol`) shared with COMGR                                                                               |
| `executable.cpp`                    | Loader integration: ISA mismatch detection, hotswap pipeline dispatch, agent compatibility bypass                                                               |
| `hip_fatbin.cpp`                    | Fat binary intercept: cross-gen ISA extraction, `e_flags` patching, early `rocr_hotswap_retarget` call                                                          |


---

## 5. Environment Variables


| Variable                   | Where Checked                                                                                           | Purpose                                                                                                                                       |
| -------------------------- | ------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| `HSA_HOTSWAP_RULES`        | `hotswap.cpp` `IsEnabled()`, `hotswap_rules.cpp` `GetCachedRules()`, `executable.cpp`, `hip_fatbin.cpp` | Path to JSON rules file. Setting this (even to `/dev/null`) enables the hotswap engine.                                                       |
| `HSA_HOTSWAP_ISA_OVERRIDE` | `hotswap.cpp` `IsIsaOverrideEnabled()`, `executable.cpp`, `hip_fatbin.cpp`                              | Target ISA for cross-gen retargeting (e.g. `gfx942`). Value `"0"` disables. Enables fat binary intercept and loader ISA compatibility bypass. |
| `HSA_HOTSWAP_COMGR_LIB`    | `hotswap_comgr_client.cpp`                                                                              | Path to `libamd_comgr.so` for hotswap operations. Falls back to `libamd_comgr.so` / `libamd_comgr.so.3` on `LD_LIBRARY_PATH`.                 |
| `HSA_HOTSWAP_DUMP`         | `hotswap_core.cpp` `ShouldDump()`                                                                       | Set to `1` to dump before/after disassembly to stderr.                                                                                        |
| `HSA_HOTSWAP_DUMP_ELF`     | `executable.cpp`                                                                                        | Path to write rewritten ELF after transformation.                                                                                             |
| `HSA_HOTSWAP_LLVM_MC`      | `hotswap.cpp` `rocr_hotswap_assemble_inst`                                                              | Path to `llvm-mc` binary for standalone assembly helper.                                                                                      |


### Enablement Logic

```
IsEnabled() → true if:
    HSA_HOTSWAP_RULES is set and non-empty
    OR
    HSA_HOTSWAP_ISA_OVERRIDE is set, non-empty, and doesn't start with '0'
```

---

## 6. Integration Points

### 6a. HIP Fat Binary Intercept (`hip_fatbin.cpp`)

When `HSA_HOTSWAP_ISA_OVERRIDE` is set:

1. **Bare ELF path:** If the code object is a standalone ELF (not a fat binary)
  and its `EF_AMDGPU_MACH` differs from the device, HIP patches `e_flags` and
   `.note` ISA strings in a static buffer, then optionally calls
   `rocr_hotswap_retarget` via `dlsym`.
2. **Fat binary fallback:** If COMGR can't find a native ISA code object, HIP
  builds `hotswap_extra_isas` (mapping like gfx942↔gfx950, gfx1250/1251 for
   CDNA targets) and re-queries the fat binary for those ISAs. If found, the
   extracted CO is patched and passed to `AddDevProgram`.

### 6b. ROCR Loader Hook (`executable.cpp`)

Gated by `#ifdef ROCR_HOTSWAP_ENABLED` (set by CMake) and `IsEnabled()` at
runtime. Inside `ExecutableImpl::LoadCodeObject`, after ELF parsing but before
`LoadSegments`:

1. **ISA detection:** Scans ELF `.note` sections (types 27 and 32) for `gfx`*
  token. Compares with `HSA_HOTSWAP_ISA_OVERRIDE` target.
2. **Agent compatibility bypass:** If agent doesn't support the code object ISA
  but hotswap is active, sets `isaOverridden = true` instead of returning an
   error.
3. **Pipeline dispatch:**
  - Cross-family: `NeedsTranspile()` → `TranspileCodeObject()` (may reallocate)
  - gfx1250 B0→A0: `RetargetCodeObjectB0A0Grow()` (always allocates new buffer)
  - Same-family: `RetargetCodeObject()` + `PatchElfIsa()`
  - Rules: `RewriteCodeObject()` (if `HSA_HOTSWAP_RULES` is set)
4. **Buffer refresh:** If transpile/B0A0 produced a new buffer, `AmdHsaCode` is
  re-initialized from the new buffer before `LoadSegments`.

### 6c. ISA Registry (`isa.cpp`)

Registers ISA entries including `gfx1250` so `IsaFromName` / `IsaSupportedByAgent`
have defined behavior for hotswap-patched code objects.

### 6d. ELF Constants (`amd_hsa_elf.h`)

Defines `EF_AMDGPU_MACH_`* values (e.g. `gfx942 = 0x42`, `gfx950 = 0x4e`,
`gfx1250 = 0x49`) used by `PatchElfIsa` and fat binary patching.

---

## 7. Core Implementation Details

### 7a. ELF ISA Patching (`PatchElfIsa`)

Implemented entirely in `hotswap.cpp` (no COMGR needed):

1. Maps `gfx*` string → `EF_AMDGPU_MACH` byte (hardcoded table)
2. Patches `e_flags` bits [7:0]
3. Scans `SHT_NOTE` sections for `NT_AMDGPU_ISA` (type 27, owner `"AMDGPU"`)
4. In-place string replacement (shorter targets are null-padded)

### 7b. ELF Parsing (`ParseElfInfo`)

Zero-dependency 64-bit ELF parser in `hotswap_core.cpp`:

- Reads section headers via `SHT_STRTAB` for names
- Locates `.text` section (caches index, offset, size, addr)
- Reads symbol tables (`SHT_SYMTAB` / `SHT_DYNSYM`)
- Returns `ElfInfo` struct consumed by trampoline growth, KD updates, etc.

### 7c. Trampoline Mechanism

For rewrites where the replacement is larger than the original instruction:

```
 Original .text:                    Trampoline region (appended to .text):
 ┌─────────────────────┐           ┌──────────────────────────┐
 │ ...                 │           │ <replacement sequence>   │
 │ s_branch trampoline ├──────────►│ ...                      │
 │ <NOP padding>       │     ┌─────┤ s_branch back            │
 │ <resume point>      │◄────┘     └──────────────────────────┘
 │ ...                 │
 └─────────────────────┘
```

- `s_branch` uses SOPP encoding: signed 16-bit dword offset (±128KB range)
- GFX9 opcode base: `0xBF820000`; GFX12: `0xBFA00000`
- `GrowElfWithTrampolines` appends trampoline bytes after existing `.text` and
updates ELF section/program header offsets and sizes

### 7d. Kernel Descriptor Updates (`UpdateKernelDescriptor`)

After adding emulation trampolines that use extra registers:

- Locates `<kernel_name>.kd` symbol in the ELF
- Patches `COMPUTE_PGM_RSRC1` field (VGPR/SGPR granularity values)
- Accounts for extra VGPRs/SGPRs needed by emulation sequences

### 7e. NOP Sled Management

`NopSled` tracks contiguous NOP regions in `.text`:

- `FindNearestSled` locates a sled near a given offset with enough space
- Used by trampoline placement when inline NOPs can absorb small expansions

### 7f. WMMA Hazard Classification

For B0→A0 patching, `ClassifyWmmaNops` returns per-mnemonic NOP requirements:

- `b0_nops`: NOP count for B0 stepping
- `a0_nops`: NOP count for A0 stepping
- A hazard exists when `a0 > b0` (A0 needs more spacing)

---

## 8. COMGR Backend (Instruction Rewriting)

All instruction-level rewriting (disassemble, opcode remap, reassemble) is
performed by **AMD COMGR** (`libamd_comgr.so`), loaded at runtime.

### Dynamic Binding

`hotswap_comgr_client.cpp` resolves two symbols via `dlopen`/`dlsym`:

- `amd_comgr_hotswap_rewrite` — the workhorse: accepts ELF + flags + ISA strings,
returns rewritten ELF
- `amd_comgr_hotswap_needs_transpile` — cross-family detection

Requires COMGR >= 3.2. Falls back gracefully if unavailable.

### Flag-Based Operations

All rewrite operations funnel through `ComgrHotswapRewrite` with different flags:


| Flag                               | Operation              | Used By                                       |
| ---------------------------------- | ---------------------- | --------------------------------------------- |
| `COMGR_HOTSWAP_FLAG_B0_TO_A0`      | gfx1250 B0→A0 patching | `RetargetCodeObjectB0A0Grow`                  |
| `COMGR_HOTSWAP_FLAG_RETARGET`      | Same-family retarget   | `RetargetCodeObject`                          |
| `COMGR_HOTSWAP_FLAG_TRANSPILE`     | Cross-family transpile | `TranspileCodeObject`                         |
| `COMGR_HOTSWAP_FLAG_REWRITE_RULES` | JSON rule application  | `RewriteCodeObject` / `RewriteCodeObjectGrow` |


### Result Reporting

`ComgrHotswapResult` provides statistics:

- `rules_matched`, `trampolines_added` (for rules/B0A0)
- `transpile_passthrough`, `transpile_renamed`, `transpile_waitcnt`,
`transpile_unsupported` (for transpile)

---

## 9. Cross-Family Transpiler (gfx1250 → gfx9)

The transpiler performs full ISA translation between GPU architecture families.
The implementation lives in COMGR (accessed via `COMGR_HOTSWAP_FLAG_TRANSPILE`);
the ROCR side provides the API surface in `transpiler.hpp`.

### Pipeline

1. **Disassemble** source `.text` using LLVM MC (GFX1250 decoder)
2. **Translate** each instruction:
  - Remap opcodes via GFX12→GFX9 tables
  - Re-encode instruction formats (SMEM, FLAT, DS, VOP3 differ)
  - Merge split wait counters (`s_wait_loadcnt` → `s_waitcnt`)
  - Widen EXEC operations for wave32→wave64
  - Promote SALU float to VALU + `v_readfirstlane`
  - Split VOPD dual-issue into two VOP instructions
  - Substitute WMMA → MFMA with lane redistribution
3. **Reassemble** translated text using LLVM MC (GFX9 assembler)
4. **Patch** `.text`, kernel descriptors, ELF metadata

### Key Challenges


| Challenge                      | Solution                                                                             |
| ------------------------------ | ------------------------------------------------------------------------------------ |
| **Wave32 → Wave64**            | Half-wave execution: `exec_hi = 0`, only lower 32 lanes active. ~50% ALU efficiency. |
| **EXEC save/restore widening** | `s_mov_b32 exec_lo, sN` → `s_mov_b32 exec_lo, sN` + `s_mov_b32 exec_hi, 0`           |
| **FLAT 96-bit → 64-bit**       | Size-reducing: extract fields, truncate offset (24→13 bits), pad with NOP            |
| **SMEM restructure**           | 6-bit opcode + 24-bit offset → 8-bit opcode + 21-bit offset                          |
| **WMMA → MFMA**                | Lane remap via `ds_permute_b32`/`ds_bpermute_b32`, supports 5 shape variants         |
| **VCC wave width**             | Wave32 VCC is 32-bit; wave64 VCC is 64-bit. Clear `vcc_hi` before VCC branches.      |
| **TTMP workgroup ID**          | GFX12 provides workgroup IDs via TTMP registers; GFX9 uses kernel descriptor setup   |


### Status

- **Passing:** 15 scalar kernels, 12 matmul shapes, 7 softmax shapes, fused attention
- **Partial:** Split-K (3/8, >256 blocks fail), multi-head attention (0/4, VGPR allocation mismatch)

---

## 10. gfx1250 B0→A0 Patching

Binary compatibility patches for gfx1250 hardware stepping differences. Invoked
via `RetargetCodeObjectB0A0Grow` / `rocr_hotswap_gfx1250_b0_to_a0_grow`.

### What Gets Patched


| Patch                | Description                                                 |
| -------------------- | ----------------------------------------------------------- |
| `s_clause`           | Insert appropriate NOPs for clause scheduling               |
| Cluster loads        | Mnemonic rewrites for cluster load instructions             |
| `ds_store_2addr`     | Convert 2-address DS stores to single-address equivalents   |
| `tensor_load_to_lds` | Patches that may require appending trampolines (ELF growth) |
| WMMA scheduling      | Insert `v_nop` instructions based on A0 cycle budgets       |
| `s_wait_dscnt`       | Wait counter bump for DS operations                         |


### ELF Growth

Unlike in-place retarget, B0→A0 always allocates a new buffer (`malloc`'d) because
trampoline insertion may extend `.text` beyond its original size. The caller must
`free()` the output buffer.

### DWARF Preservation

After ELF growth, debug information is preserved:

- `.debug_`* sections remain valid
- `.debug_line` sizes are updated
- `__hotswap_tramp_*` symbols are added for trampolines
- Kernel symbols are preserved

---

## 11. Test Infrastructure

### Unit Tests (`hotswap_test.cpp`)

- JSON rule parsing (valid, invalid, edge cases)
- `EncodeSBranch` / `EncodeSNop` encoding correctness
- B0→A0 on minimal synthetic ELFs: patch counts, exact byte values
- Trampoline branching: `s_branch` offset verification
- Grow-ELF path: GFX12 branch encoding, `s_wait_dscnt` bump

### B0→A0 HSACO Tests (`test_b0a0_hsaco.cpp`)

- Real Triton `.hsaco` files from `hsaco_data/`
- Pre/post `llvm-objdump -d` mnemonic counts
- **Idempotence:** second pass produces 0 patches, identical bytes
- Non-target instruction stability (`s_mov_b32`, `v_fma_f32` unchanged)
- WMMA hazard NOP insertion verification
- Patches 6–9 (WMMA decompose, FP8 clamp, scale16) disassembly checks

### COMGR Transpiler Tests (`test_transpiler_comgr.cpp`)

- `needs_transpile`: true for gfx1250→gfx942, false for same-ISA
- Single-instruction and multi-instruction kernel translation
- Disassembly string checks (e.g. `global_load_b32` → `global_load_dword`)
- Invalid ISA strings must not crash
- **Concurrent safety:** 4 threads performing simultaneous B0→A0 rewrites

### Dataflow Analysis Tests (`test_dataflow.cpp`)

- VGPR def/use tracking (including WMMA tuples)
- CFG construction from branch instructions
- Liveness at instruction indices
- Scratch VGPR allocator (reuse dead regs, spill above KD range)

### DWARF Tests (`test_dwarf.cpp`)

- All 4 reference `.hsaco` files
- `.debug_`* section preservation after grow
- `__hotswap_tramp_*` symbol creation
- Kernel symbol preservation
- Minimal ELF without DWARF: no crash

### GPU Execution Tests (`test_suite.hip`, `test_multi_new.hip`)

- 20 kernels: scalar compute, matmul (12 shapes), softmax (7 shapes), attention,
split-K, multi-head attention
- `hipModuleLoad` + launch + host reference comparison
- Exact match for integer ops, tight float tolerances

### Fuzz Harness (`fuzz_hotswap.cpp`)

- AFL persistent mode / libFuzzer compatible
- Feeds arbitrary bytes through `ParseElfInfo` + `rocr_hotswap_gfx1250_b0_to_a0_grow`
- Goal: crash-finding for ELF parsing robustness

### Python Tests

- `test_transpiler.py`: Assembly validity via `llvm-mc` (source assembles on
gfx1250, translated assembles on gfx950)
- `test_transpiler_e2e.py`: Full pipeline: assemble gfx1250 → disasm → transpile
→ assemble gfx950; includes flat+saddr→global conversion

---

## 12. Build System

### CMake Option

```cmake
option(ROCR_ENABLE_HOTSWAP "Enable ROCm HotSwap load-time ISA rewriting" OFF)
```

When `ON`:

- Adds `hotswap/*.cpp` sources to `libhsa-runtime64`
- Adds `hotswap/` include directory
- Links `dl` (for `dlopen`)
- Defines `ROCR_HOTSWAP_ENABLED=1` (gates `#ifdef` in `executable.cpp`)

### Standalone Test Build (`hotswap/CMakeLists.txt`)

Builds a `rocr-hotswap` static library plus test executables:

- `hotswap_test`, `b0a0_hsaco_test`, `b0a0_retarget_tool`
- `dwarf_test`, `dataflow_test`, `transpiler_comgr_test`
- `fuzz_hotswap`

Tests that need COMGR check `HSA_HOTSWAP_COMGR_LIB` and skip gracefully if
unset.

---

## 13. Key Data Structures

### `ElfInfo` / `ElfSection` / `ElfSymbol` (`hotswap_shared_types.h`)

Lightweight ELF view shared between ROCR and COMGR:

```cpp
struct ElfInfo {
  std::vector<ElfSection> sections;
  std::vector<ElfSymbol> symbols;
  int text_section_idx = -1;
  uint64_t text_offset = 0;   // .text byte offset in ELF
  uint64_t text_size = 0;
  uint64_t text_addr = 0;     // .text virtual address
};
```

### `DecodedInst` (`hotswap.hpp`)

Instruction after disassembly:

```cpp
struct DecodedInst {
  uint64_t offset;           // Byte offset within .text
  uint32_t size;             // 4 or 8 bytes for AMDGPU
  std::string mnemonic;
  std::vector<uint8_t> bytes;
};
```

### `RewriteResult` (`hotswap.hpp`)

Returned by all rewrite operations:

```cpp
struct RewriteResult {
  hsa_status_t status;
  uint32_t rules_matched;
  uint32_t trampolines_added;
};
```

### `RewriteRule` (`hotswap_rules.hpp`)

A single JSON rewrite rule:

- `match_mnemonic`, `operands` (wildcard/immediate/regclass), `match_kernel`,
`match_offset`
- Replace payload: `MnemonicSwap`, `AsmReplace`, or `ByteReplace`
- Optional `extra_vgprs` / `extra_sgprs` for KD adjustment

### `Trampoline` (`trampoline.hpp`)

Out-of-line replacement sequence:

```cpp
struct Trampoline {
  uint64_t original_offset;
  uint64_t original_size;
  std::vector<uint8_t> bytes;  // Replacement + s_branch back
};
```

### `ComgrHotswapFlags` / `ComgrHotswapResult` (`hotswap_comgr_client.hpp`)

Flag enum for operation selection; result struct with per-operation statistics.

### `TranspileStats` (`transpiler.hpp`)

Transpile-specific counters:

```cpp
struct TranspileStats {
  uint32_t total_instructions;
  uint32_t translated_passthrough;  // No change needed
  uint32_t translated_renamed;      // Mnemonic renamed
  uint32_t translated_waitcnt;      // Wait counter merged
  uint32_t translated_exec;         // EXEC widened
  uint32_t unsupported_skipped;     // NOPped
  uint32_t assembly_errors;
};
```

---

## 14. Execution Flow Walkthrough

### Scenario: gfx950 binary on gfx942 hardware

1. **HIP:** `ExtractFatBinaryUsingCOMGR` queries for `gfx942` CO → not found
2. **HIP:** `HSA_HOTSWAP_ISA_OVERRIDE` is set → builds `hotswap_extra_isas`
  including `gfx950`
3. **HIP:** Re-queries → finds gfx950 CO → copies to static buffer
4. **HIP:** Patches `e_flags` (`0x4e` → `0x42`), `.note` (`gfx950` → `gfx942`)
5. **HIP:** `dlsym("rocr_hotswap_retarget")` → calls into ROCR
6. **ROCR:** `RetargetCodeObject` → COMGR `RETARGET` flag → NOP gfx950-only
  opcodes, re-encode if needed
7. **ROCR:** `AddDevProgram` passes patched CO to CLR
8. **ROCR Loader:** `LoadCodeObject` → `IsEnabled()` true → applies any additional
  JSON rules
9. **ROCR:** `LoadSegments` copies final code to GPU memory
10. **GPU:** Executes retargeted code (99.7% identical encoding, 0.3% NOPed)

### Scenario: gfx1250 binary on gfx942 hardware

1. **HIP:** Finds gfx1250 CO via `hotswap_extra_isas`, patches metadata
2. **ROCR Loader:** `IsEnabled()` true, ISA override active
3. **ROCR:** `NeedsTranspile("gfx1250", "gfx942")` → true
4. **ROCR:** `TranspileCodeObject` → COMGR `TRANSPILE` flag → full
  disassemble→translate→reassemble
5. **ROCR:** Output may be larger (wave size adaptation) → new buffer allocated
6. **ROCR:** `AmdHsaCode` reinitialized from new buffer
7. **ROCR:** `LoadSegments` → GPU executes transpiled code

---

## 15. Known Limitations and Gaps

### Implementation

- `**TranslateInstruction` is declared but not defined** in the ROCR tree — it
would fail to link if called. The implementation lives in COMGR.
- `**TranspileStats` fields are partially populated** from COMGR results:
`translated_exec` and `assembly_errors` are not mapped from the COMGR result
struct.
- **FP4/FP8 emulation** for gfx950→gfx942 is approximate (scale+truncate+clamp
instead of exact E2M1 quantization). RMSNorm accuracy degrades ~50%.
- **Multi-kernel ELFs** are handled as one kernel per code object in some transpile
paths; split-K uses separate ELFs as a workaround.
- `**s_branch` range:** ±128KB (signed 16-bit dword offset). Very large kernels
may need `s_setpc_b64` (12 bytes) for trampoline reach.

### Documentation vs Code Drift

- `hotswap-architecture.md` sections 3a–3b describe in-process LLVM MC inside
`hotswap.cpp`; the current implementation delegates to COMGR. Treat those
sections as historical/conceptual.
- `gfx1250-on-gfx950-analysis.md` references files (`transpiler.cpp`,
`opcode_tables.h`, `wave_adapt.cpp`, `wmma_to_mfma.cpp`) that are not in the
`hotswap/` directory — they live inside COMGR.

### Cross-Family Transpile

- Multi-head attention: VGPR allocation mismatch (0/4 passing)
- Split-K with >256 blocks: fails (3/8 passing)
- SWMMAC (sparse matrix) emulation: not implemented
- DPP8→DPP translation: partial
- MIMG re-encoding: not fully covered

---

## 16. Glossary


| Term             | Definition                                                                                                            |
| ---------------- | --------------------------------------------------------------------------------------------------------------------- |
| **B0→A0**        | Hardware stepping patch: fixes for gfx1250 B0 vs A0 silicon differences (also used for WMMA cycle budget terminology) |
| **COMGR**        | AMD's Code Object Manager (compiler support library wrapping LLVM MC)                                                 |
| **CDNA**         | AMD's compute GPU architecture (gfx9xx series: MI300X, MI355X)                                                        |
| **RDNA**         | AMD's graphics GPU architecture (gfx1xxx series)                                                                      |
| **Code Object**  | AMD GPU ELF binary containing kernel machine code                                                                     |
| **Fat Binary**   | Container bundling code objects for multiple ISAs                                                                     |
| **ISA Override** | Environment variable directing hotswap to accept foreign-ISA code objects                                             |
| **KD**           | Kernel Descriptor: 64-byte header preceding each kernel's `.text`                                                     |
| **MFMA**         | Matrix Fused Multiply-Add (CDNA matrix instruction, wave64)                                                           |
| **NOP Sled**     | Region of `s_nop` instructions available for trampoline placement                                                     |
| **Retarget**     | Same-family ISA translation (e.g. gfx950→gfx942, shared encodings)                                                    |
| **Transpile**    | Cross-family ISA translation (e.g. gfx1250→gfx942, full re-encoding)                                                  |
| **Trampoline**   | Out-of-line code patch: `s_branch` to replacement, `s_branch` back                                                    |
| **WMMA**         | Wave Matrix Multiply-Accumulate (RDNA matrix instruction, wave32)                                                     |


---

*Last updated: 2026-04-14. Generated from commit history
`e1575520fd55..2fcef92b79` on branch `users/powderluv/rocm-hotswap`.*