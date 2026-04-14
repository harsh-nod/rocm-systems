#!/usr/bin/env python3
"""Extract gfx1250 code objects from hipBLASLt TensileLite for hotswap testing.

Runs TensileCreateLibrary on the pre-tuned gfx1250 logic YAMLs and organizes
the resulting .co (ELF code objects) into tensile_co_data/ with a manifest.

Usage:
    python3 extract_gfx1250_kernels.py \
        --rocm-libraries /workspace/rocm-libraries \
        --output ./tensile_co_data \
        --cxx-compiler /opt/rocm/bin/amdclang++ \
        --llvm-objdump /opt/rocm/llvm/bin/llvm-objdump \
        --jobs 8
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


HIPBLASLT_LOGIC_REL = (
    "projects/hipblaslt/library/src/amd_detail/rocblaslt/src"
    "/Tensile/Logic/asm_full/gfx1250"
)
HIPSPARSELT_LOGIC_REL = (
    "projects/hipsparselt/library/src/hcc_detail/rocsparselt/src"
    "/spmm/Tensile/Logic/asm_full/gfx1250"
)
TENSILELITE_REL = "projects/hipblaslt/tensilelite"


def find_logic_yamls(logic_dir: Path) -> list[Path]:
    return sorted(logic_dir.rglob("*.yaml"))


def run_tensile_create_library(
    logic_dir: Path,
    output_dir: Path,
    cxx_compiler: str,
    tensilelite_dir: Path,
    jobs: int,
) -> bool:
    cmd = [
        sys.executable, "-m", "Tensile.TensileCreateLibrary",
        str(logic_dir),
        str(output_dir),
        "HIP",
        "--architecture", "gfx1250",
        "--cxx-compiler", cxx_compiler,
        "--jobs", str(jobs),
        "--code-object-version", "5",
        "--no-compress",
    ]
    env = os.environ.copy()
    env["PYTHONPATH"] = str(tensilelite_dir) + ":" + env.get("PYTHONPATH", "")
    print(f"Running: {' '.join(cmd)}")
    result = subprocess.run(
        cmd, cwd=str(tensilelite_dir), env=env,
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print(f"FAILED (exit {result.returncode}):")
        print(result.stderr[-2000:] if len(result.stderr) > 2000 else result.stderr)
        return False
    for line in result.stdout.splitlines():
        if any(k in line for k in ["Total time", "Total kernels", "DONE"]):
            print(f"  {line.strip()}")
    return True


def analyze_code_object(co_path: Path, llvm_objdump: str) -> dict:
    """Extract metadata from a code object via llvm-objdump."""
    info = {
        "file": co_path.name,
        "size_bytes": co_path.stat().st_size,
        "kernels": [],
        "instruction_counts": {},
    }
    try:
        result = subprocess.run(
            [llvm_objdump, "-d", str(co_path)],
            capture_output=True, text=True, timeout=30,
        )
        disasm = result.stdout
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return info

    kernel_names = set()
    for m in re.finditer(r'^[0-9a-f]+ <(.+?)>:', disasm, re.MULTILINE):
        name = m.group(1)
        if not name.startswith("label_") and not name.startswith("."):
            kernel_names.add(name)
    info["kernels"] = sorted(kernel_names)

    feature_patterns = {
        "v_wmma": r'\bv_wmma\w*',
        "v_swmmac": r'\bv_swmmac\w*',
        "global_load": r'\bglobal_load\w*',
        "global_store": r'\bglobal_store\w*',
        "ds_load": r'\bds_load\w*',
        "ds_store": r'\bds_store\w*',
        "s_wait": r'\bs_wait\w*',
        "buffer_load": r'\bbuffer_load\w*',
        "buffer_store": r'\bbuffer_store\w*',
        "v_fma": r'\bv_fma\w*',
        "v_cvt": r'\bv_cvt\w*',
        "v_pk": r'\bv_pk_\w*',
    }
    for key, pattern in feature_patterns.items():
        info["instruction_counts"][key] = len(re.findall(pattern, disasm))

    total_lines = sum(1 for line in disasm.splitlines()
                      if re.match(r'^\s+\S', line))
    info["instruction_counts"]["total"] = total_lines

    return info


def infer_category(yaml_name: str) -> tuple[str, dict]:
    """Infer data type and category from YAML filename."""
    meta = {"data_types": [], "features": []}
    name_lower = yaml_name.lower()

    type_map = {
        "_hhs_": ("FP16", "HHS"),
        "_bbs_": ("BF16", "BBS"),
        "_sss_": ("FP32", "SSS"),
        "_ddd_": ("FP64", "DDD"),
        "_i8": ("INT8", "I8"),
        "_f8": ("FP8", "F8"),
        "_b8": ("BF8", "B8"),
        "_f6": ("FP6", "F6"),
        "_b6": ("BF6", "B6"),
        "_f4": ("FP4", "F4"),
        "_mx": ("MX", "MX"),
    }
    for pattern, (dtype, tag) in type_map.items():
        if pattern in name_lower:
            meta["data_types"].append(dtype)

    if "sparse" in name_lower or "spm" in name_lower or "spbm" in name_lower or "spam" in name_lower:
        meta["features"].append("sparse")
    if "grad" in name_lower:
        meta["features"].append("gradient")
    if "bias" in name_lower:
        meta["features"].append("bias")
    if "activation" in name_lower:
        meta["features"].append("activation")

    if not meta["data_types"]:
        meta["data_types"] = ["unknown"]
    return meta.get("data_types", ["unknown"])[0], meta


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rocm-libraries", required=True,
                        help="Path to rocm-libraries checkout")
    parser.add_argument("--output", required=True,
                        help="Output directory for extracted code objects")
    parser.add_argument("--cxx-compiler", default="/opt/rocm/bin/amdclang++")
    parser.add_argument("--llvm-objdump", default="/opt/rocm/llvm/bin/llvm-objdump")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--max-yamls", type=int, default=0,
                        help="Limit number of logic YAMLs (0=all)")
    args = parser.parse_args()

    rocm_libs = Path(args.rocm_libraries)
    output = Path(args.output)
    tensilelite_dir = rocm_libs / TENSILELITE_REL

    hipblaslt_logic = rocm_libs / HIPBLASLT_LOGIC_REL
    if not hipblaslt_logic.exists():
        print(f"ERROR: Logic directory not found: {hipblaslt_logic}")
        return 1

    output.mkdir(parents=True, exist_ok=True)
    co_dir = output / "code_objects"
    co_dir.mkdir(exist_ok=True)

    print(f"=== Extracting gfx1250 kernels from hipBLASLt ===")
    yamls = find_logic_yamls(hipblaslt_logic)
    if args.max_yamls > 0:
        yamls = yamls[:args.max_yamls]
    print(f"Found {len(yamls)} logic YAML files")

    with tempfile.TemporaryDirectory(prefix="tensile_extract_") as tmpdir:
        tmp_logic = Path(tmpdir) / "logic"
        tmp_logic.mkdir()
        gfx_dir = tmp_logic / "gfx1250"
        gfx_dir.mkdir()
        gridbased = gfx_dir / "GridBased"
        gridbased.mkdir()

        for y in yamls:
            rel = y.relative_to(hipblaslt_logic)
            dst = gfx_dir / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(y, dst)

        tmp_out = Path(tmpdir) / "out"
        print(f"\nRunning TensileCreateLibrary ({len(yamls)} logic files)...")
        ok = run_tensile_create_library(
            tmp_logic, tmp_out, args.cxx_compiler, tensilelite_dir, args.jobs,
        )
        if not ok:
            print("ERROR: TensileCreateLibrary failed")
            return 1

        co_files = sorted(tmp_out.rglob("*.co")) + sorted(tmp_out.rglob("*.hsaco"))
        print(f"\nGenerated {len(co_files)} code object files")

        manifest = {"source": "hipblaslt-tensilelite", "architecture": "gfx1250",
                     "code_objects": []}

        for co in co_files:
            dst = co_dir / co.name
            shutil.copy2(co, dst)
            print(f"  Analyzing {co.name}...")
            info = analyze_code_object(dst, args.llvm_objdump)
            _, meta = infer_category(co.name)
            info["meta"] = meta
            manifest["code_objects"].append(info)

    manifest_path = output / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"\n=== Summary ===")
    print(f"Code objects: {len(manifest['code_objects'])}")
    print(f"Output dir:   {co_dir}")
    print(f"Manifest:     {manifest_path}")

    total_kernels = sum(len(co["kernels"]) for co in manifest["code_objects"])
    total_wmma = sum(co["instruction_counts"].get("v_wmma", 0)
                     for co in manifest["code_objects"])
    total_swmmac = sum(co["instruction_counts"].get("v_swmmac", 0)
                       for co in manifest["code_objects"])
    total_insts = sum(co["instruction_counts"].get("total", 0)
                      for co in manifest["code_objects"])
    print(f"Total kernels:      {total_kernels}")
    print(f"Total instructions: {total_insts}")
    print(f"Total v_wmma:       {total_wmma}")
    print(f"Total v_swmmac:     {total_swmmac}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
