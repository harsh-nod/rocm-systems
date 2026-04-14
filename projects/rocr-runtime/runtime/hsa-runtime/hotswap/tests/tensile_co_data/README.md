# gfx1250 TensileLite Kernel Extraction & Hotswap Testing

## What this is

147 gfx1250 code objects (ELF binaries) extracted from hipBLASLt's
TensileLite pre-tuned logic, used as test inputs for the hotswap
B0→A0 patching, ISA retargeting, and cross-family transpiler.

- **1,021 kernels** across all code objects
- **3.1M total instructions**, including **8,693 WMMA** instructions
- Data types: BF8, FP8, INT8, MX, FP16, BF16, FP32, FP64
- Kernel features: bias, gradient, activation, scaled-accumulate

## Files

```
tensile_co_data/
  manifest.json       # per-file metadata: kernels, instruction counts, data types
  test_results.csv    # per-file hotswap test results
  code_objects/       # 146 .co + 1 .hsaco raw AMDGPU ELF files
```

## Prerequisites

### System requirements

- ROCm 7.2+ installed at `/opt/rocm` (provides `amdclang++`, `llvm-objdump`, `libamd_comgr.so`)
- Python 3.10+
- CMake 3.16+

### Build hotswap library

```bash
cd rocm-systems/projects/rocr-runtime/runtime/hsa-runtime/hotswap
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_OBJDUMP_BIN=/opt/rocm/llvm/bin/llvm-objdump
cmake --build build -j$(nproc)
```

### Build rocisa (only needed to re-extract kernels)

```bash
pip install nanobind
cd rocm-libraries/projects/hipblaslt

# Install msgpack-cxx (header-only C++ library)
cd /tmp && git clone --depth 1 --branch cpp-6.1.1 https://github.com/msgpack/msgpack-c.git
cd msgpack-c && cmake -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && cmake --install build
cd -

cmake --preset rocisa -DCMAKE_PREFIX_PATH="/opt/rocm;/usr/local"
cmake --build build -j$(nproc)
```

### Install TensileLite (only needed to re-extract kernels)

```bash
cd rocm-libraries/projects/hipblaslt/tensilelite

# Copy the built rocisa .so into the package
cp ../build/tensilelite/rocisa/lib/rocisa.cpython-*.so rocisa/

# Create __init__.py to bridge native extension
cat > rocisa/__init__.py << 'PYEOF'
import sys as _sys
from rocisa import rocisa as _native
for _name in dir(_native):
    if not _name.startswith('_'):
        _attr = getattr(_native, _name)
        if type(_attr).__name__ == 'module':
            _sys.modules[f'rocisa.{_name}'] = _attr
from rocisa.rocisa import *
PYEOF

# Install Python deps and TensileLite
pip install msgpack joblib simplejson ujson orjson yappi
pip install -e . --no-deps
```

## Running the tests

### Run pre-extracted code objects (no TensileLite needed)

```bash
cd rocm-systems/projects/rocr-runtime/runtime/hsa-runtime/hotswap

# Run all tests
./build/tensile_co_test

# Verbose mode (per-file results)
./build/tensile_co_test -v

# Custom directory
./build/tensile_co_test -d /path/to/code_objects/
```

The test validates each code object:

1. **ELF valid** -- confirms the file is a well-formed ELF binary
2. **B0A0 no crash** -- runs `rocr_hotswap_gfx1250_b0_to_a0_grow()` without segfault
3. **B0A0 idempotent** -- a second B0A0 pass produces 0 additional patches
4. **Disasm valid** -- `llvm-objdump -d` succeeds on the (patched) output
5. **WMMA count** -- reports number of `v_wmma*` instructions found

Note: B0A0 patching requires COMGR >= 3.2 (`libamd_comgr.so` with
`amd_comgr_hotswap_rewrite` symbol). With older COMGR, the test still
passes (0 patches applied, input passed through unchanged).

### Re-extract kernels from rocm-libraries

```bash
cd rocm-systems/projects/rocr-runtime/runtime/hsa-runtime/hotswap

python3 tests/extract_gfx1250_kernels.py \
    --rocm-libraries /workspace/rocm-libraries \
    --output tests/tensile_co_data \
    --cxx-compiler /opt/rocm/bin/amdclang++ \
    --llvm-objdump /opt/rocm/llvm/bin/llvm-objdump \
    --jobs 32

# For a quick smoke test with fewer YAMLs:
python3 tests/extract_gfx1250_kernels.py \
    --rocm-libraries /workspace/rocm-libraries \
    --output /tmp/quick_test \
    --max-yamls 5 \
    --jobs 8
```

The extraction script:

1. Finds all gfx1250 logic YAMLs under `rocm-libraries/projects/hipblaslt/.../Logic/asm_full/gfx1250/`
2. Runs `TensileCreateLibrary` with `--no-compress` to produce raw ELF `.co` files
3. Analyzes each with `llvm-objdump` to extract kernel names and instruction counts
4. Writes `manifest.json` and copies code objects to `code_objects/`

### Rebuild and run from scratch (full pipeline)

```bash
# 1. Build hotswap
cd rocm-systems/projects/rocr-runtime/runtime/hsa-runtime/hotswap
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_OBJDUMP_BIN=/opt/rocm/llvm/bin/llvm-objdump
cmake --build build -j$(nproc)

# 2. Extract kernels (requires TensileLite installed per above)
python3 tests/extract_gfx1250_kernels.py \
    --rocm-libraries /workspace/rocm-libraries \
    --output tests/tensile_co_data \
    --jobs 32

# 3. Run tests
./build/tensile_co_test -v
```

## Environment variables

| Variable | Purpose | Example |
|---|---|---|
| `HSA_HOTSWAP_COMGR_LIB` | Path to COMGR shared library | `/opt/rocm/lib/libamd_comgr.so` |
| `HSA_HOTSWAP_RULES` | Path to JSON rewrite rules file | `rules/gfx950_to_gfx942.json` |
| `HSA_HOTSWAP_ISA_OVERRIDE` | Target ISA for cross-gen retarget | `gfx942` |

## CSV columns

| Column | Description |
|---|---|
| `file` | Code object filename |
| `size_bytes` | File size in bytes |
| `num_kernels` | Number of GPU kernels in the code object |
| `total_instructions` | Total disassembled instruction count |
| `elf_valid` | OK if valid ELF header |
| `b0a0_ok` | OK if B0→A0 patching did not crash |
| `b0a0_patches` | Number of instructions patched by B0→A0 |
| `idempotent` | OK if second B0→A0 pass yields 0 patches |
| `disasm_valid` | OK if llvm-objdump can disassemble |
| `wmma_count` | Number of `v_wmma*` instructions |
| `v_fma_count` | Number of `v_fma*` instructions |
| `v_cvt_count` | Number of `v_cvt*` instructions |
| `v_pk_count` | Number of `v_pk_*` instructions |
| `ds_load_count` | Number of `ds_load*` instructions |
| `ds_store_count` | Number of `ds_store*` instructions |
| `global_load_count` | Number of `global_load*` instructions |
| `global_store_count` | Number of `global_store*` instructions |
| `buffer_load_count` | Number of `buffer_load*` instructions |
| `buffer_store_count` | Number of `buffer_store*` instructions |
| `s_wait_count` | Number of `s_wait*` instructions |
| `data_types` | Inferred data types (FP16, BF8, FP8, etc.) |
| `features` | Inferred kernel features (bias, gradient, etc.) |
