#!/bin/bash
################################################################################
#
# Integration test: gfx1250 B0→A0 patches on Triton-compiled kernels
#
# Prerequisites:
#   - Docker container with ROCm 7.2+ and Triton gfx1250 support
#   - Built b0a0_retarget_tool in the hotswap build directory
#   - llvm-objdump available in PATH (from ROCm or LLVM install)
#
# Usage:
#   bash tests/test_b0a0_triton.sh [build_dir]
#
# The script:
#   1. Locates Triton gfx1250 kernel .hsaco files
#   2. Runs b0a0_retarget_tool on each
#   3. Disassembles the output with llvm-objdump
#   4. Verifies no B0-only instructions remain
#
################################################################################

set -euo pipefail

BUILD_DIR="${1:-build}"
TOOL="${BUILD_DIR}/b0a0_retarget_tool"
TRITON_EXAMPLES="/home/harmenon/dockerx/triton/third_party/amd/python/examples/gluon"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

# Check prerequisites
if [[ ! -x "$TOOL" ]]; then
  echo "ERROR: b0a0_retarget_tool not found at '$TOOL'"
  echo "Build it first: cmake -B $BUILD_DIR && cmake --build $BUILD_DIR"
  exit 1
fi

if ! command -v llvm-objdump &>/dev/null; then
  echo "ERROR: llvm-objdump not found in PATH"
  exit 1
fi

PASS=0
FAIL=0
TOTAL=0

# Find .hsaco files — check common locations
HSACO_DIRS=(
  "$HOME/.triton/cache"
  "/tmp/triton_cache"
  "$TRITON_EXAMPLES"
)

HSACO_FILES=()
for dir in "${HSACO_DIRS[@]}"; do
  if [[ -d "$dir" ]]; then
    while IFS= read -r -d '' f; do
      HSACO_FILES+=("$f")
    done < <(find "$dir" -name "*.hsaco" -print0 2>/dev/null)
  fi
done

if [[ ${#HSACO_FILES[@]} -eq 0 ]]; then
  echo "WARNING: No .hsaco files found. Run Triton kernels first to generate them."
  echo "Example: python -c 'import triton; ...'"
  echo ""
  echo "Searching in: ${HSACO_DIRS[*]}"
  exit 0
fi

echo "Found ${#HSACO_FILES[@]} .hsaco file(s) to test"
echo ""

for hsaco in "${HSACO_FILES[@]}"; do
  TOTAL=$((TOTAL + 1))
  basename=$(basename "$hsaco")
  outfile="$TMPDIR/${basename%.hsaco}_patched.hsaco"

  echo "--- Testing: $basename ---"

  # Apply patches
  patch_count=$("$TOOL" "$hsaco" "$outfile" 2>/dev/null)
  echo "  Patches applied: $patch_count"

  # Disassemble and check for remaining B0-only instructions
  disasm=$(llvm-objdump -d "$outfile" 2>/dev/null || true)

  found_bad=0
  for pattern in "cluster_load" "2addr" "s_clause"; do
    matches=$(echo "$disasm" | grep -ci "$pattern" || true)
    if [[ "$matches" -gt 0 ]]; then
      echo "  FAIL: found $matches remaining '$pattern' instruction(s)"
      found_bad=1
    fi
  done

  if [[ "$found_bad" -eq 0 ]]; then
    echo "  PASS: no B0-only instructions remain"
    PASS=$((PASS + 1))
  else
    echo "  FAIL: B0-only instructions still present"
    FAIL=$((FAIL + 1))
  fi
  echo ""
done

echo "========================================"
echo "Results: $PASS/$TOTAL passed, $FAIL failed"
echo "========================================"

if [[ "$FAIL" -gt 0 ]]; then
  exit 1
fi
exit 0
