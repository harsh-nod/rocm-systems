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

import argparse
import copy
import csv
import re
import sys
from abc import abstractmethod
from collections import OrderedDict
from pathlib import Path
from typing import Any, Optional, TextIO

import pandas as pd
import yaml

import config
from rocprof_compute_soc.soc_base import OmniSoC_Base
from utils import file_io, parser, schema, tty
from utils.logger import (
    console_debug,
    console_error,
    console_log,
    console_warning,
    demarcate,
)
from utils.roofline_calc import validate_roofline_csv
from utils.utils import (
    build_kernel_name_to_id,
    compute_operator_prefix_stats,
    get_uuid,
    impute_counters_iteration_multiplex,
    is_workload_empty,
    merge_counters_spatial_multiplex,
    process_torch_trace_output,
)

# the build-in config to list kernel names purpose only
TOP_STATS_BUILD_IN_CONFIG: OrderedDict[int, dict[str, Any]] = OrderedDict([
    (
        0,
        {
            "id": 0,
            "title": "Top Kernels",
            "data source": [
                {"raw_csv_table": {"id": 1, "source": "pmc_kernel_top.csv"}}
            ],
        },
    ),
    (
        1,
        {
            "id": 1,
            "title": "Dispatch List",
            "data source": [
                {"raw_csv_table": {"id": 2, "source": "pmc_dispatch_info.csv"}}
            ],
        },
    ),
])


# ------------------------------------
# Helper functions for join_prof()
# ------------------------------------


def test_df_column_equality(df: pd.DataFrame) -> bool:
    """Test if all columns in dataframe are equal."""
    return df.eq(df.iloc[:, 0], axis=0).all(1).all()


def detect_missing_counters(
    df: pd.DataFrame,
    workload_dir: Path,
    join_type: str,
    iteration_multiplexing: Optional[int],
) -> None:
    """Detect missing counter values in joined dataframe.

    Args:
        df: Joined dataframe to check
        workload_dir: Path to workload directory
        join_type: Type of join performed ('kernel' or 'grid')
        iteration_multiplexing: Iteration multiplexing value (None if disabled)
    """
    group_labels = ["Kernel_Name"]
    if join_type == "grid":
        group_labels.append("Grid_Size")

    num_files = len(list(workload_dir.glob("perfmon/*.txt")))
    kernels_with_missing_counters = []
    for _, groups in df.groupby(group_labels):
        if groups["Dispatch_ID"].nunique() < num_files:
            kernel_name = groups.iloc[0]["Kernel_Name"]
            kernels_with_missing_counters.append(kernel_name)

    if kernels_with_missing_counters:
        kernels_with_missing_counters = list(set(kernels_with_missing_counters))
        console_warning(
            "join_prof",
            (
                f"Insufficient number of kernel calls for kernels: "
                f"{', '.join(kernels_with_missing_counters)} "
                f"to collect all counters using iteration multiplexing. "
                f"Please use kernel filtering and exclude the above kernels "
                f"or turn off iteration multiplexing."
            ),
        )
        with open(workload_dir / "profiling_config.yaml", "a") as f:
            yaml.dump(
                {"kernels_with_missing_counters": kernels_with_missing_counters}, f
            )


class OmniAnalyze_Base:
    def __init__(
        self, args: argparse.Namespace, supported_archs: dict[str, str]
    ) -> None:
        self.__args = args
        self._runs: OrderedDict[str, schema.Workload] = OrderedDict()
        self._arch_configs: dict[str, schema.ArchConfig] = {}
        self.__supported_archs = supported_archs
        self._output: Optional[TextIO] = None
        self.__socs: Optional[dict[str, OmniSoC_Base]] = None

    def get_args(self) -> argparse.Namespace:
        return self.__args

    def get_profiling_config(self) -> dict[str, Any]:
        return self._profiling_config

    def set_soc(self, omni_socs: dict[str, OmniSoC_Base]) -> None:
        self.__socs = omni_socs

    def get_socs(self) -> Optional[dict[str, OmniSoC_Base]]:
        return self.__socs

    @demarcate
    def spatial_multiplex_merge_counters(self, df: pd.DataFrame) -> pd.DataFrame:
        return merge_counters_spatial_multiplex(df)

    @demarcate
    def iteration_multiplex_impute_counters(
        self, df: pd.DataFrame, policy: str
    ) -> pd.DataFrame:
        return impute_counters_iteration_multiplex(df, policy)

    @demarcate
    def generate_configs(
        self,
        arch: str,
        config_dir: str,
        list_stats: bool,
        filter_metrics: Optional[list[str]],
        sys_info: pd.Series,
    ) -> dict[str, schema.ArchConfig]:
        single_panel_config = file_io.is_single_panel_config(
            config_dir, self.__supported_archs
        )

        ac = schema.ArchConfig()
        if list_stats:
            ac.panel_configs = TOP_STATS_BUILD_IN_CONFIG
        else:
            arch_panel_config = [
                config_dir if single_panel_config else str(f"{config_dir}/{arch}")
            ]
            # Use restructured perf metrics in TUI analyze mode
            if self.get_args().tui and arch in ["gfx942", "gfx950"]:
                arch_panel_config.append(
                    str(
                        config.rocprof_compute_home
                        / "rocprof_compute_tui"
                        / "utils"
                        / arch
                    )
                )
            ac.panel_configs = file_io.load_panel_configs(arch_panel_config)

        # TODO: filter_metrics should/might be one per arch
        parser.build_dfs(
            arch_configs=ac, filter_metrics=filter_metrics, sys_info=sys_info
        )
        self._arch_configs[arch] = ac
        return self._arch_configs

    @demarcate
    def list_torch_operators(self) -> None:
        """
        List PyTorch operators with hierarchy, numbering, and durations.

        Displays each operator with hierarchy, operator/kernel
        durations, and numbering.
        """
        workload_path = (
            self.__args.path[0][0]
            if isinstance(self.__args.path[0], list)
            else self.__args.path[0]
        )
        process_torch_trace_output(workload_path)
        torch_trace_dir = Path(workload_path) / "torch_trace"
        all_files = list(torch_trace_dir.glob("*.csv"))
        # Load each CSV, compute prefix_stats once, and derive
        # total duration (highest first)
        file_data: list[
            tuple[Path, pd.DataFrame, dict[str, tuple[float, int]], float]
        ] = []
        for f in all_files:
            try:
                df = pd.read_csv(f)
                ps = compute_operator_prefix_stats(df)
                total_ms = sum(dur for key, (dur, _) in ps.items() if "/" not in key)
                file_data.append((f, df, ps, total_ms))
            except Exception as e:
                console_error(f"Failed to read operator from {f.name}: {e}")
        # Sort by total duration in descending order
        file_data.sort(key=lambda x: x[3], reverse=True)
        # Use default kernel verbosity = 1
        kernel_verbose = getattr(self.__args, "kernel_verbose", 1)
        kernel_name_to_id = build_kernel_name_to_id(
            [df for _, df, _, _ in file_data], kernel_verbose
        )
        # print() is intentional: banner lines are display output that wraps
        # tty.show_torch_operator_hierarchy (also print-based); console_log
        # would prepend an INFO prefix in colored log modes.
        print(f"\n{'=' * 80}")
        print(f"PyTorch Operators in: {workload_path}")
        if kernel_name_to_id:
            print("Kernel (id N) can be used with -k for filtering.")
        print(f"{'=' * 80}\n")
        operator_count = 0
        for idx, (f, df, ps, total_ms) in enumerate(file_data, start=1):
            tty.show_torch_operator_hierarchy(
                str(f.name).replace(".csv", ""),
                df,
                index=idx,
                kernel_name_to_id=kernel_name_to_id,
                kernel_verbose=kernel_verbose,
                prefix_stats=ps,
            )
            operator_count += 1

        if not operator_count:
            console_warning(
                "No PyTorch operator data found. "
                "Please ensure profiling was done with --torch-trace option."
            )

        print(f"\n{'=' * 80}")
        print(f"Total: {operator_count} operators")
        print(f"{'=' * 80}\n")

    @demarcate
    def load_options(self, normalization_filter: Optional[str]) -> None:
        args = self.get_args()
        profiling_config = self.get_profiling_config()
        target_filter = normalization_filter or args.normal_unit

        for arch_config in self._arch_configs.values():
            parser.build_metric_value_string(
                arch_config.dfs,
                arch_config.dfs_type,
                target_filter,
                profiling_config,
            )
        # Error checking for multiple runs and multiple kernel filters
        if args.gpu_kernel and (len(args.path) != len(args.gpu_kernel)):
            if len(args.gpu_kernel) == 1:
                args.gpu_kernel *= len(args.path)
            else:
                console_error(
                    "analysis"
                    "The number of -k/--kernel doesn't match the number of --dir."
                )

    @demarcate
    def initalize_runs(
        self, normalization_filter: Optional[str] = None
    ) -> OrderedDict[str, schema.Workload]:
        args = self.get_args()

        if getattr(args, "list_torch_operators", False):
            self.list_torch_operators()
            sys.exit(0)

        def get_sysinfo_path(data_path: str) -> Optional[str]:
            return (
                data_path
                if args.nodes is None and not args.spatial_multiplexing
                else file_io.find_1st_sub_dir(data_path)
            )

        # load required configs
        for path_info in args.path:
            sysinfo_path = get_sysinfo_path(path_info[0])
            if sysinfo_path:
                sys_info = file_io.load_sys_info(f"{sysinfo_path}/sysinfo.csv")
                arch = sys_info.iloc[0]["gpu_arch"]
                self.generate_configs(
                    arch,
                    args.config_dir,
                    args.list_stats,
                    args.filter_metrics,
                    sys_info.iloc[0],
                )

        self.load_options(normalization_filter)

        for path_info in args.path:
            # FIXME:
            #    For regular single node case, load sysinfo.csv directly
            #    For multi-node, either the default "all", or specified some,
            #    pick up the one in the 1st sub_dir. We could fix it properly later.
            w = schema.Workload()
            sysinfo_path = get_sysinfo_path(path_info[0])
            if sysinfo_path:
                w.sys_info = file_io.load_sys_info(f"{sysinfo_path}/sysinfo.csv")
                if not getattr(args, "no_roof", False):
                    # Validate roofline CSV before loading

                    is_valid, error_msg = validate_roofline_csv(sysinfo_path)

                    if is_valid:
                        try:
                            roofline_df = pd.read_csv(f"{sysinfo_path}/roofline.csv")
                            w.roofline_peaks = roofline_df
                        except Exception as e:
                            console_error(
                                "roofline",
                                f"Failed to load roofline.csv: {e}",
                                exit=False,
                            )
                            w.roofline_peaks = pd.DataFrame()
                    else:
                        console_log(
                            "roofline",
                            f"Roofline analysis skipped: {error_msg}",
                        )
                        w.roofline_peaks = pd.DataFrame()
                else:
                    w.roofline_peaks = pd.DataFrame()

                arch = w.sys_info.iloc[0]["gpu_arch"]
                socs = self.get_socs()
                if socs and arch in socs:
                    mspec = socs[arch]._mspec
                    if args.specs_correction:
                        w.sys_info = parser.correct_sys_info(
                            mspec, args.specs_correction
                        )
                w.avail_ips = w.sys_info["ip_blocks"].item().split("|")
                w.dfs = copy.deepcopy(self._arch_configs[arch].dfs)
                w.dfs_type = self._arch_configs[arch].dfs_type
                self._runs[path_info[0]] = w

        return self._runs

    @demarcate
    def sanitize(self) -> None:
        """Perform sanitization of inputs"""
        args = self.get_args()

        if args.tui:
            return

        if not args.path:
            console_error("The following arguments are required: -p/--path")

        # verify not accessing parent directories
        if ".." in str(args.path):
            console_error(
                "Access denied. Cannot access parent directories in path (i.e. ../)"
            )

        # ensure absolute path
        seen_paths: set[str] = set()
        for dir_info in args.path:
            full_path = Path(dir_info[0]).absolute().resolve()
            dir_info[0] = str(full_path)

            if not full_path.is_dir():
                console_error(
                    "analysis", f"Invalid directory {full_path}\nPlease try again."
                )
            # validate profiling data

            if dir_info[0] in seen_paths:
                console_error("analysis", "You cannot provide the same path twice.")
            seen_paths.add(dir_info[0])

        self._profiling_config: dict[str, Any] = file_io.load_profiling_config(
            args.path[0][0]
        )
        profiling_config = self.get_profiling_config()

        for dir_info in args.path:
            if not any([
                args.nodes,
                args.list_nodes,
                args.spatial_multiplexing,
                profiling_config.get("iteration_multiplexing"),
            ]):
                is_workload_empty(dir_info[0])

        # FIXME:
        #   The proper location of this func should be in pre_processing().
        #   However, because of reading soc depends on sys spec, and sys
        #   spec depends on sys_info. And we read sys_info too early so we
        # . can not do it now. There should be a way to make it simpler.
        if args.list_nodes:
            # NB:
            #   There are 2 ways to do it: one is doing like the below, checking
            #   sub dirs only as we assume the profiling stage generate sub dirs
            #   with node name. The 2nd way would be checkign host name in each
            #   sub dir and very those.
            nodes = [
                subdir.name
                for subdir in Path(args.path[0][0]).iterdir()
                if subdir.is_dir()
            ]
            print("Node list:", "  ".join(nodes))
            sys.exit(0)

        # Validate --nodes option against workload structure
        if args.nodes is not None:
            for dir_info in args.path:
                workload_path = dir_info[0]
                valid_nodes = file_io.get_valid_nodes(workload_path)

                if not valid_nodes:
                    # Single-node workload: sysinfo.csv is in root, not in
                    # subdirectories
                    console_error(
                        "analysis",
                        f"The workload at '{workload_path}' is single-node "
                        "(sysinfo.csv is in the root directory).\n"
                        "The --nodes option is only supported for multi-node "
                        "workloads where each node subdirectory contains its "
                        "own sysinfo.csv.\n"
                        "Remove the --nodes option to analyze this "
                        "single-node workload.",
                    )

                # If specific nodes are provided (not empty list), validate them
                if args.nodes:
                    invalid_nodes = [n for n in args.nodes if n not in valid_nodes]
                    if invalid_nodes:
                        console_error(
                            "analysis",
                            f"Invalid node(s): {', '.join(invalid_nodes)}\n"
                            f"Valid nodes for '{workload_path}': "
                            f"{', '.join(valid_nodes)}\n"
                            "Each valid node must be a subdirectory "
                            "containing sysinfo.csv.",
                        )

        # Ensure analysis output does not overwrite existing files
        if args.output_name:
            if not re.match(r"^[A-Za-z0-9_-]+$", args.output_name):
                console_error(
                    "analysis",
                    "Analysis output file/folder name must "
                    "contain only alphanumeric characters "
                    "or underscores (_), hyphens (-).",
                )

            path_to_check = args.output_name
            if args.output_format in ("txt", "db"):
                path_to_check += f".{args.output_format}"

            if Path(path_to_check).exists():
                console_error(
                    f"Analysis output file/folder {path_to_check} already exists. "
                    "Please choose a different name."
                )

        # Check if any kernel's counters are missing due to iteration multiplexing
        if (
            profiling_config.get("iteration_multiplexing") is not None
            and profiling_config.get("kernels_with_missing_counters") is not None
        ):
            missing_kernels = profiling_config.get("kernels_with_missing_counters")
            console_warning(
                "analysis",
                (
                    "The following kernels have missing counter data "
                    "due to iteration multiplexing and should be filtered out: "
                    f"{', '.join(missing_kernels)}"
                ),
            )

        if profiling_config.get("iteration_multiplexing") is not None:
            console_log(
                "analysis",
                (
                    "Profiling data was collected using iteration multiplexing.\n\t"
                    "Metrics are calculated based on partially available counter data."
                ),
            )

    @demarcate
    def join_prof(
        self, workload_dir: Path, out: Optional[str] = None
    ) -> Optional[pd.DataFrame]:
        """Join separated rocprof runs into single pmc_perf.csv.

        Args:
            workload_dir: Path to workload directory containing CSV files
            out: Optional output file path (defaults to workload_dir/pmc_perf.csv)

        Returns:
            DataFrame if called programmatically, None if saving to file
        """
        output_file = out or str(workload_dir / "pmc_perf.csv")

        # Load profiling config from THIS workload directory (not args)
        profiling_config = file_io.load_profiling_config(str(workload_dir))
        format_rocprof = profiling_config.get("format_rocprof_output", "rocpd")
        iteration_multiplexing = profiling_config.get("iteration_multiplexing", None)
        join_type = profiling_config.get("join_type", "grid")
        kokkos_trace = profiling_config.get("kokkos_trace", False)

        # handle rocpd format
        if format_rocprof == "rocpd":
            # Vertically concat (by rows) results_*.csv into pmc_perf.csv
            result_files = list(workload_dir.glob("results_*.csv"))

            with open(output_file, "w", newline="") as outfile:
                writer = None
                for file in result_files:
                    with open(file, newline="") as infile:
                        reader = csv.reader(infile)
                        header = next(reader)
                        # Write header only once
                        if writer is None:
                            writer = csv.writer(outfile)
                            writer.writerow(header)
                        for row in reader:
                            writer.writerow(row)

            console_debug(f"Created file: {output_file}")

            if iteration_multiplexing is not None:
                df = pd.read_csv(output_file)
                detect_missing_counters(
                    df, workload_dir, join_type, iteration_multiplexing
                )

            return None

        # Collect files to process - normalize to Path objects
        files: list[Path] = []

        csv_patterns = ["pmc_perf_*.csv", "SQ_*.csv", "SQC_*.csv"]
        files = [
            file for pattern in csv_patterns for file in workload_dir.glob(pattern)
        ]

        if kokkos_trace:
            # remove marker api trace outputs from this list
            files = [f for f in files if not f.name.endswith("_marker_api_trace.csv")]

        # Process files and create joined dataframe
        df = None
        for i, file in enumerate(files):
            current_df = pd.read_csv(file)

            if current_df.empty:
                console_warning("join_prof", f"Empty dataframe from {file}")
                continue

            if join_type == "kernel":
                key = current_df.groupby("Kernel_Name").cumcount()
                current_df["key"] = current_df.Kernel_Name + " - " + key.astype(str)
            elif join_type == "grid":
                key = current_df.groupby(["Kernel_Name", "Grid_Size"]).cumcount()
                current_df["key"] = (
                    current_df["Kernel_Name"].astype(str)
                    + " - "
                    + current_df["Grid_Size"].astype(str)
                    + " - "
                    + key.astype(str)
                )
            else:
                console_error(
                    "join_prof",
                    f"{join_type} is an unrecognized option for --join-type",
                )

            if df is None:
                df = current_df
            else:
                # join by unique index of kernel
                df = pd.merge(
                    df, current_df, how="inner", on="key", suffixes=("", f"_{i}")
                )

        if df is None or df.empty:
            console_warning("join_prof", "No data available after processing all files")
            return None

        # TODO: check for any mismatch in joins
        duplicate_cols = {
            "GPU_ID": [col for col in df.columns if col.startswith("GPU_ID")],
            "Grid_Size": [col for col in df.columns if col.startswith("Grid_Size")],
            "Workgroup_Size": [
                col for col in df.columns if col.startswith("Workgroup_Size")
            ],
            "LDS_Per_Workgroup": [
                col for col in df.columns if col.startswith("LDS_Per_Workgroup")
            ],
            "Scratch_Per_Workitem": [
                col for col in df.columns if col.startswith("Scratch_Per_Workitem")
            ],
            "SGPR": [col for col in df.columns if col.startswith("SGPR")],
        }

        # Check for vgpr counter in ROCm < 5.3
        if "vgpr" in df.columns:
            duplicate_cols["vgpr"] = [
                col for col in df.columns if col.startswith("vgpr")
            ]
        # Check for vgpr counter in ROCm >= 5.3
        else:
            duplicate_cols["Arch_VGPR"] = [
                col for col in df.columns if col.startswith("Arch_VGPR")
            ]
            duplicate_cols["Accum_VGPR"] = [
                col for col in df.columns if col.startswith("Accum_VGPR")
            ]

        for key, cols in duplicate_cols.items():
            current_df = df[cols]
            if not test_df_column_equality(current_df):
                console_warning(
                    "join_prof",
                    f"Detected differing {key} values while joining pmc_perf.csv",
                )
            else:
                console_debug("join_prof", f"Successfully joined {key} in pmc_perf.csv")

        # now, we can:
        #   A) throw away any of the "boring" duplicates
        columns_to_remove = [
            # rocprofv2 headers
            "GPU_ID_",
            "Grid_Size_",
            "Workgroup_Size_",
            "LDS_Per_Workgroup_",
            "Scratch_Per_Workitem_",
            "vgpr_",
            "Arch_VGPR_",
            "Accum_VGPR_",
            "SGPR_",
            "Dispatch_ID_",
            "Queue_ID",
            "Queue_Index",
            "PID",
            "TID",
            "SIG",
            "OBJ",
            "Correlation_ID_",
            "Wave_Size_",
            # rocscope specific merged counters, keep original
            "dispatch_",
            # extras
            "sig",
            "queue-id",
            "queue-index",
            "pid",
            "tid",
            "fbar",
        ]

        df = df[
            [
                col
                for col in df.columns
                if not any(col.startswith(prefix) for prefix in columns_to_remove)
            ]
        ]

        #   B) any timestamps that are _not_ the duration,
        #      which is the one we care about
        timestamp_patterns = ["DispatchNs", "CompleteNs", "HostDuration"]

        df = df[
            [
                col
                for col in df.columns
                if not any(pattern in col for pattern in timestamp_patterns)
            ]
        ]

        #   C) sanity check the name and key
        name_cols = [col for col in df.columns if "Kernel_Name" in col]
        if not name_cols:
            return df

        for col in name_cols[1:]:
            assert (df[name_cols[0]] == df[col]).all()

        df = df.drop(columns=name_cols[1:])

        # now take the median of the durations
        start_cols = [col for col in df.columns if "Start_Timestamp" in col]
        end_cols = [col for col in df.columns if "End_Timestamp" in col]

        # compute mean mean timestamps
        if start_cols and end_cols:
            mean_start = df[start_cols].mean(axis=1)
            mean_end = df[end_cols].mean(axis=1)

            # Replace with consolidated timestamps
            df = df.drop(columns=start_cols + end_cols)
            df["Start_Timestamp"] = mean_start
            df["End_Timestamp"] = mean_end

        # finally, join the drop key
        if "key" in df.columns:
            df = df.drop(columns=["key"])

        console_debug("join_prof", "Checking for missing counter values...")

        if iteration_multiplexing is not None:
            detect_missing_counters(df, workload_dir, join_type, iteration_multiplexing)

        # save to file
        df.to_csv(output_file, index=False)
        return None

    # ----------------------------------------------------
    # Required methods to be implemented by child classes
    # ----------------------------------------------------
    @abstractmethod
    def pre_processing(self) -> None:
        """Perform initialization prior to analysis."""
        console_debug("analysis", "prepping to do some analysis")
        console_log("analysis", "deriving rocprofiler-compute metrics...")
        args = self.get_args()

        # initalize output file
        if args.output_format == "txt":
            output_filename = args.output_name or f"rocprof_compute_{get_uuid()}"
            output_filename += ".txt"
            self._output = open(output_filename, "w+")
            console_warning("analysis", f"Created file: {output_filename}")
        elif args.output_format == "stdout":
            self._output = sys.stdout

        # initalize runs
        self._runs = self.initalize_runs()

        # set filters
        filter_configs = [
            (args.gpu_kernel, "filter_kernel_ids"),
            (args.gpu_id, "filter_gpu_ids"),
            (args.gpu_dispatch_id, "filter_dispatch_ids"),
            (args.nodes, "filter_nodes"),
        ]

        for filter_list, attr_name in filter_configs:
            if not filter_list:
                continue

            # Extend single filter to match all paths
            if len(filter_list) == 1 and len(args.path) > 1:
                filter_list *= len(args.path)

            # Apply filters to workloads
            for path_info, filter_value in zip(args.path, filter_list):
                setattr(self._runs[path_info[0]], attr_name, filter_value)

        # Join pmc_perf_*.csv or results_*.csv files if needed
        for path_info in args.path:
            workload_dir = Path(path_info[0])

            # Detect CSV format: merged vs separate
            pmc_perf = workload_dir / "pmc_perf.csv"
            pmc_perf_files = list(workload_dir.glob("pmc_perf_*.csv"))
            results_files = list(workload_dir.glob("results_*.csv"))

            if pmc_perf.exists():
                # Already merged (old workload or re-running analyze)
                console_debug(f"Using existing {pmc_perf}")
            elif pmc_perf_files or results_files:
                # New format: separate CSVs need joining
                files_desc = "pmc_perf_*.csv" if pmc_perf_files else "results_*.csv"
                console_log(f"Joining {files_desc} for {workload_dir}...")
                self.join_prof(workload_dir, out=str(pmc_perf))
                console_log(f"Created {pmc_perf}")
            else:
                # No PMC data found - error out immediately with clear message
                console_error(
                    f"No profiling data found in {workload_dir}.\n"
                    f"Expected: pmc_perf.csv or pmc_perf_*.csv or results_*.csv\n"
                    f"Please run 'rocprof-compute profile' first."
                )

    @abstractmethod
    def run_analysis(self) -> None:
        """Run analysis."""
        console_debug("analysis", "generating analysis")
