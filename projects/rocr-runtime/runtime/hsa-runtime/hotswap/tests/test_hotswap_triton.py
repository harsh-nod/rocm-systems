#!/usr/bin/env python3
"""Simple Triton kernel to test hotswap is intercepting code objects."""

import torch
import triton
import triton.language as tl
import os
import sys
import json


@triton.jit
def vector_add_kernel(
    x_ptr, y_ptr, out_ptr,
    N,
    BLOCK_SIZE: tl.constexpr,
):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < N
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, x + y, mask=mask)


def get_gfx_target():
    """Detect the GPU's gfx target string."""
    props = torch.cuda.get_device_properties(0)
    gcn = props.gcnArchName  # e.g. "gfx950" or "gfx942:sramecc+:xnack-"
    return gcn.split(":")[0]


def make_rules_file(gfx: str, path: str):
    """Create a hotswap rules file with a benign s_nop match.

    Every GPU kernel ends with at least one s_nop or s_waitcnt.
    This rule replaces s_nop with s_nop — semantically a no-op,
    but hotswap will log that it matched and applied the rule.
    """
    rules = {
        "version": 1,
        "target": f"amdgcn-amd-amdhsa--{gfx}",
        "rules": [
            {
                "name": "test_nop_identity",
                "match": {"mnemonic": "s_nop"},
                "replace_asm": "s_nop 0",
            },
        ],
    }
    with open(path, "w") as f:
        json.dump(rules, f, indent=2)
    return rules


def main():
    if not torch.cuda.is_available():
        print("No GPU available")
        sys.exit(1)

    gfx = get_gfx_target()
    print(f"GPU target: {gfx}")

    # --- Step 1: Run without hotswap to verify kernel correctness ---
    N = 1024
    x = torch.randn(N, device="cuda", dtype=torch.float32)
    y = torch.randn(N, device="cuda", dtype=torch.float32)
    out = torch.empty_like(x)

    grid = lambda meta: (triton.cdiv(N, meta["BLOCK_SIZE"]),)
    vector_add_kernel[grid](x, y, out, N, BLOCK_SIZE=256)

    expected = x + y
    if torch.allclose(out, expected):
        print("PASS: vector_add result is correct")
    else:
        print("FAIL: vector_add result mismatch")
        sys.exit(1)

    # --- Step 2: Check if hotswap rules are active ---
    rules_env = os.environ.get("HSA_HOTSWAP_RULES")
    if rules_env:
        print(f"HSA_HOTSWAP_RULES={rules_env}")
        print("Hotswap is active — check stderr for 'hotswap:' messages")
    else:
        # Generate a rules file for the user
        rules_path = "/tmp/hotswap_test_rules.json"
        rules = make_rules_file(gfx, rules_path)
        print(f"\nNo HSA_HOTSWAP_RULES set. To test hotswap, re-run with:")
        print(f"  export HSA_HOTSWAP_RULES={rules_path}")
        print(f"  python {sys.argv[0]}")
        print(f"\nGenerated rules file ({rules_path}):")
        print(json.dumps(rules, indent=2))


if __name__ == "__main__":
    main()
