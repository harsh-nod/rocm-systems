##############################################################################
# MIT License
#
# Copyright (c) 2021 - 2025 Advanced Micro Devices, Inc. All Rights Reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

##############################################################################

"""
Common utilities used by BOTH profile and analyze code paths.

This module contains utilities that are called by both profiling and analysis
operations, including:
- Version/environment detection
- Identifiers (UUID, rank)
- Configuration parsing
- Time formatting
- General helpers

Functions in this module must be dependency-light (stdlib only or vendored deps).
"""

import argparse
import io
import os
import re
import shutil
import uuid
from pathlib import Path
from typing import Any, Optional

import yaml

import config
from utils.logger import console_debug, console_error

# Constants
METRIC_ID_RE = re.compile(pattern=r"^\d{1,2}(?:\.\d{1,2}){0,2}$")
NS_TO_MS = 1.0 / 1_000_000.0


def detect_rocprof(args: argparse.Namespace) -> str:
    """Detect loaded rocprof version. Resolve path and set cmd globally."""
    # Import here to avoid circular dependency
    from utils import utils_profile

    # Default is rocprofiler-sdk
    if os.environ.get("ROCPROF", "rocprofiler-sdk") == "rocprofiler-sdk":
        if not Path(args.rocprofiler_sdk_tool_path).exists():
            console_error(
                "Could not find rocprofiler-sdk tool at "
                f"{args.rocprofiler_sdk_tool_path}"
            )
        utils_profile.rocprof_cmd = "rocprofiler-sdk"
        console_debug(f"rocprof_cmd is {utils_profile.rocprof_cmd}")
        console_debug(f"rocprofiler_sdk_tool_path is {args.rocprofiler_sdk_tool_path}")
    else:
        # If ROCPROF is not set to rocprofiler-sdk
        utils_profile.rocprof_cmd = os.environ["ROCPROF"]
        rocprof_path = shutil.which(utils_profile.rocprof_cmd)
        if not rocprof_path:
            console_error(
                f"Unable to resolve path to {utils_profile.rocprof_cmd} binary. "
                "Please verify installation or set ROCPROF "
                "environment variable with full path."
            )
        rocprof_path = str(Path(rocprof_path.rstrip("\n")).resolve())
        console_debug(f"rocprof_cmd is {str(utils_profile.rocprof_cmd)}")
        console_debug(f"ROC Profiler: {rocprof_path}")
    return utils_profile.rocprof_cmd


def format_time(seconds: float) -> str:
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    secs = int(seconds % 60)
    parts: list[str] = []

    if hours > 0:
        parts.append(f"{hours} hour{'s' if hours != 1 else ''}")
    if minutes > 0:
        parts.append(f"{minutes} minute{'s' if minutes != 1 else ''}")
    if secs > 0 or not parts:
        parts.append(f"{secs} second{'s' if secs != 1 else ''}")

    if len(parts) <= 1:
        return parts[0] if parts else "0 seconds"

    return ", ".join(parts[:-1]) + f" and {parts[-1]}"


def get_panel_alias() -> dict[str, str]:
    def load_yaml(filepath: str) -> dict[str, Any]:
        """Load YAML file and return as dictionary."""
        with open(filepath) as f:
            return yaml.safe_load(f)

    panel_yaml = load_yaml(
        f"{config.rocprof_compute_home}/rocprof_compute_soc/analysis_configs/gfx9_config_template.yaml"
    )
    return {
        panel["panel_alias"]: str(panel["panel_id"]) for panel in panel_yaml["panels"]
    }


def get_rank() -> Optional[str]:
    rank_env_vars = [
        "SLURM_PROCID",
        "FLUX_TASK_RANK",
        "PMI_RANK",
        "PMIX_RANK",
        "PALS_RANKID",
        "OMPI_COMM_WORLD_RANK",
        "MV2_COMM_WORLD_RANK",
        "MPI_RANKID",
        "MPI_LOCALRANKID",
        "MPI_RANK",
    ]
    for env_var in rank_env_vars:
        value = os.environ.get(env_var)
        if value is not None:
            return value

    return None


def get_submodules(package_name: str) -> list[str]:
    """List all submodules for a target package"""
    import importlib
    import pkgutil

    submodules: list[str] = []

    # walk all submodules in target package
    package = importlib.import_module(package_name)
    for _, name, _ in pkgutil.walk_packages(package.__path__):
        pretty_name = name.split("_", 1)[1].replace("_", "")
        # ignore base submodule, add all other
        if pretty_name != "base":
            submodules.append(pretty_name)

    return submodules


def get_uuid(length: int = 8) -> str:
    return uuid.uuid4().hex[:length]


def get_version(rocprof_compute_home: Path) -> dict[str, str]:
    """Return ROCm Compute Profiler versioning info"""
    # Import here to avoid circular dependency
    from utils.utils_profile import capture_subprocess_output

    # semantic version info - note that version file(s) can reside in
    # two locations depending on development vs formal install
    search_dirs = [rocprof_compute_home, rocprof_compute_home.parent]
    found = False
    version_dir: Optional[Path] = None
    VER = "unknown"
    SHA = "unknown"
    MODE = "unknown"

    for directory in search_dirs:
        version_file = directory / "VERSION"
        try:
            with open(version_file) as file:
                VER = file.read().replace("\n", "")
                found = True
                version_dir = directory
                break
        except Exception:
            pass
    if not found:
        console_error(f"Cannot find VERSION file at {search_dirs}")

    # git version info
    if version_dir is not None:
        try:
            success, output = capture_subprocess_output(
                ["git", "-C", version_dir, "log", "--pretty=format:%h", "-n", "1"],
            )
            if success:
                SHA = output
                MODE = "dev"
            else:
                raise Exception(output)
        except Exception:
            try:
                sha_file = version_dir / "VERSION.sha"
                with open(sha_file) as file:
                    SHA = file.read().replace("\n", "")
                    MODE = "release"
            except Exception:
                pass

    return {"version": VER, "sha": SHA, "mode": MODE}


def get_version_display(version: str, sha: str, mode: str) -> str:
    """Pretty print versioning info"""
    buf = io.StringIO()
    print("-" * 40, file=buf)
    print(f"rocprofiler-compute version: {version} ({mode})", file=buf)
    print(f"Git revision:     {sha}", file=buf)
    print("-" * 40, file=buf)
    return buf.getvalue()


def parse_sets_yaml(arch: str) -> dict[str, Any]:
    filename = (
        config.rocprof_compute_home
        / "rocprof_compute_soc"
        / "profile_configs"
        / "sets"
        / f"{arch}_sets.yaml"
    )
    with open(filename) as file:
        content = file.read()
    data = yaml.safe_load(content)

    sets_data = data.get("sets", [])

    sets_info: dict[str, Any] = {}
    for set_item in sets_data:
        set_option = set_item.get("set_option", "")
        if set_option:
            sets_info[set_option] = set_item
    return sets_info


def replace_env(name: str) -> str:
    def env(match: re.Match[str]) -> str:
        var_name = match.group(1)
        return os.environ.get(var_name, "")  # Default to empty string if not found

    # Replace %env{VAR}% with environment variable values
    pattern = re.compile(r"%env{([^}]+)}%")

    return pattern.sub(env, name)


def replace_rank(name: str) -> str:
    def rank(match: re.Match[str]) -> str:
        value = get_rank()
        if value is not None:
            return value + match.group(1)  # preserve trailing slash
        else:
            return ""  # Ignore %rank% and trailing slash

    # Replace %rank% (and optional trailing slash) with MPI process rank
    pattern = re.compile(r"%rank%(/?)")

    return pattern.sub(rank, name)
