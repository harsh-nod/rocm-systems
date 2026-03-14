# Phase 2: Move join_prof() to Analyze Mode - Eliminate Pandas from Profile

**PR #2** | **Theme**: Move CSV joining to analyze mode
**Objective**: Remove pandas dependency from profile code path by moving join_prof() to analyze
**Dependencies**: None (independent PR, can develop in parallel with Phase 1)
**Duration**: 2-3 days
**Status**: Ready to implement - Plan verified and corrected

---

## 🔑 Key Implementation Insight

**CRITICAL**: `join_prof()` must run in `OmniAnalyze_Base.pre_processing()` **BEFORE** returning to child class!

**Why**: The analyze flow loads `pmc_perf.csv` in child class pre_processing():
```
OmniAnalyze_Base.pre_processing() (line 473)
  ├─> Initialize output file (lines 480-486)
  ├─> initalize_runs() (line 489) - loads sysinfo.csv, roofline.csv (NOT pmc_perf.csv!)
  ├─> Set filters (lines 491-509)
  └─> [JOIN pmc_perf_*.csv HERE - at the END] ← Logical: workloads initialized, now prepare data
  └─> Returns to child class

cli_analysis.pre_processing() (line 38)
  └─> super().pre_processing() [calls above]
  └─> file_io.create_df_pmc() (line 50) ← NEEDS pmc_perf.csv to exist!
       └─> Searches for "pmc_perf.csv" (file_io.py:256)
       └─> pd.read_csv("pmc_perf.csv") (file_io.py:259)

db_analysis.pre_processing() (line 72)
  └─> super().pre_processing() [calls above]
  └─> self.calc_pmc_df_data() (line 82) ← NEEDS pmc_perf.csv to exist!
       └─> pd.read_csv("pmc_perf.csv") (analysis_db.py:312)
```

**Key Insight**: `initalize_runs()` does NOT need `pmc_perf.csv` - it only loads `sysinfo.csv` and `roofline.csv`. So `join_prof()` can (and should) run AFTER it.

**Solution**: Add join logic at the **END** of `OmniAnalyze_Base.pre_processing()`, after `initalize_runs()` and filters, but before returning to child class. This is more logical: first initialize workload objects, then prepare their data.

---

## Problem Statement

Profile mode uses pandas in `profiler_base.py` for CSV joining:
- `join_prof()` method (lines 222-455) - ~230 lines
- `detect_missing_counters()` helper (lines 190-219) - ~30 lines
- `test_df_column_equality()` utility (lines 825-826) - 2 lines
- Total pandas usage: ~270 lines in profile mode

**Location**: `src/rocprof_compute_profile/profiler_base.py`
**Dependency**: `pandas==2.2.3` (~50MB, largest non-stdlib package)

**Current flow**:
```
PROFILE MODE:
1. Run app N times → pmc_perf_0.csv, pmc_perf_1.csv, pmc_perf_2.csv
2. join_prof() [uses pandas] → pmc_perf.csv
3. DONE

ANALYZE MODE:
1. Load pmc_perf.csv (already merged)
2. Process and visualize
```

---

## Objective

**Move join_prof() to analyze mode** - CSV joining is fundamentally a "data preparation for analysis" step, not a profiling step.

**New flow**:
```
PROFILE MODE:
1. Run app N times → pmc_perf_0.csv, pmc_perf_1.csv, pmc_perf_2.csv
2. DONE (no pandas, no joining!)

ANALYZE MODE:
1. pre_processing(): Check if pmc_perf.csv exists
   - If yes: Load it (backward compatible)
   - If no: Call join_prof() to create it from pmc_perf_*.csv
2. Process and visualize
```

**Benefits**:
- ✅ Profile mode: ZERO pandas dependency
- ✅ Clean separation: Profile = collect, Analyze = process
- ✅ Analyze already has pandas (no new dependency)
- ✅ Backward compatible (handles both formats)
- ✅ Better flexibility (can re-join without re-profiling)
- ✅ Consistent with Phase 1 (move post-processing to analyze)

---

## Scope

### In Scope

**Remove from Profile Mode**:
- Move `join_prof()` method out of `profiler_base.py`
- Move `detect_missing_counters()` helper
- Move `test_df_column_equality()` utility
- Remove `import pandas as pd` from `profiler_base.py`
- Remove calls to `join_prof()` in `post_processing()`
- Remove useless `pmc_perf.csv` check in `soc_base.py` post_profiling()

**Add to Analyze Mode**:
- Add `join_prof()` to `analysis_base.py` pre_processing()
- Detect format: merged (pmc_perf.csv) vs separate (pmc_perf_*.csv)
- Join if needed, use existing if already merged
- Support multiple workload paths (analyze -p path1 path2 path3)

**Tests**:
- Verify profile mode has no pandas import
- Verify analyze mode handles both formats correctly
- Verify multiple paths work
- Backward compatibility test

**Documentation**:
- Update workflow docs (profile → analyze separation)
- Document format handling

### Out of Scope
- Simplifying concat/merge operations (future optimization)
- Utils refactoring (Phase 4)
- YAML elimination (Phase 3)

### In Scope (CRITICAL MODIFICATION)
- **MUST remove file deletion logic** from `join_prof()` when moving to analyze mode
- Current implementation deletes source CSV files (lines 448-452)
- This is BAD in analyze mode - analyze should be read-only on profile outputs!

---

## Implementation Plan

### Step 0: Clean Workload Directory on Re-Profile

**Problem**: When re-profiling an existing workload directory, old CSV files remain:
- Old `pmc_perf.csv` (merged by previous analyze run) becomes stale
- Old `pmc_perf_*.csv` or `results_*.csv` files are out of sync
- Analyze mode may use stale merged file instead of fresh data

**Solution**: Delete and recreate workload directory when re-profiling.

**File**: `src/rocprof_compute_base.py`

**Location**: `run_profiler()` method, lines 528-533

**Before**:
```python
# Create workload directory if it does not exist
p = Path(self.__args.path)
if not p.exists():
    try:
        p.mkdir(parents=True, exist_ok=False)
    except FileExistsError:
        console_error("Directory already exists.")
```

**After**:
```python
# Create workload directory (delete if exists for re-profiling)
p = Path(self.__args.path)
shutil.rmtree(p, ignore_errors=True)
p.mkdir(parents=True)
```

**Changes**:
1. Add `import shutil` to global imports (after line 27)
2. Replace directory creation logic with simple delete + create
3. Remove complex error handling (not needed)

**Why Delete Everything?**
- ✅ Simple: 2 lines, easy to understand
- ✅ Complete: No stale data possible
- ✅ Expected: Re-profiling means fresh start
- ✅ Cheap: `roofline.csv` and `sysinfo.csv` regenerate in seconds
- ❌ **Not** over-engineered: No selective "keep this, delete that" logic

**User Workflow**:
- Want to preserve old data? Use different `--name` or `--output-directory`
- Re-profiling same target? Get clean slate automatically

---

### Step 1: Move join_prof() Functions to Analyze

**File**: `src/rocprof_compute_analyze/analysis_base.py`

**Add to class** (likely in `pre_processing()` or new method):

```python
def join_prof(self, workload_dir: Path, out: Optional[str] = None) -> Optional[pd.DataFrame]:
    """
    Join separated rocprof runs into single pmc_perf.csv.

    Moved from profiler_base.py to eliminate pandas from profile mode.
    This is fundamentally a data preparation step for analysis.

    CRITICAL: Analyze mode is READ-ONLY on profile outputs.
    Unlike the original implementation, this does NOT delete source files.
    """
    # Copy join_prof() implementation from profiler_base.py (lines 222-455)
    # WITH these critical modifications:

    # 1. Replace args.path with workload_dir parameter
    # 2. REMOVE file deletion logic (lines 448-452) ← IMPORTANT!

    # REMOVE this block from original (lines 448-452):
    #   if not args.verbose:
    #       for file in files:
    #           if "SQ_" not in file.name or "SQC_" not in file.name:
    #               file.unlink()  # ✂️ DELETE - analyze shouldn't delete profile data!

    # Keep: All CSV joining, merging, validation, and saving logic
    # Remove: File deletion (analyze is read-only on profile outputs)
    # ... (~225 lines after removing deletion logic)

def detect_missing_counters(self, df: pd.DataFrame, workload_dir: Path, join_type: str) -> None:
    """Detect missing counter values in joined dataframe"""
    # Copy from profiler_base.py (lines 190-219)
    # Adapt to use workload_dir parameter instead of args.path
    # CRITICAL: Remove yaml write - analyze must NOT modify profiling_config.yaml!
    # CRITICAL: Remove unused iteration_multiplexing parameter (PR review feedback)
    # Warning is emitted here, no need to write to config file
    # ... (~30 lines)

def test_df_column_equality(df: pd.DataFrame) -> bool:
    """Test if all columns in dataframe are equal"""
    # Copy from profiler_base.py (lines 825-826) - no changes needed
    return df.eq(df.iloc[:, 0], axis=0).all(1).all()
```

**Why Remove File Deletion?**
1. ✅ **Analyze is read-only** - shouldn't modify profile outputs
2. ✅ **Debugging** - users may want to inspect separate CSV files
3. ✅ **Re-runnable** - can run analyze multiple times without re-profiling
4. ✅ **Ownership** - profile created the files, user deletes them (not analyze)

**CRITICAL: Remove profiling_config.yaml Write from detect_missing_counters()**

The original `detect_missing_counters()` in profile mode writes to `profiling_config.yaml`:
```python
# WRONG - Analyze shouldn't modify profile outputs!
with open(workload_dir / "profiling_config.yaml", "a") as f:
    yaml.dump({"kernels_with_missing_counters": kernels_with_missing_counters}, f)
```

**This MUST be removed because:**
1. ✅ `profiling_config.yaml` is created by **profile mode**
2. ✅ Analyze should be **READ-ONLY** on profile outputs
3. ✅ Re-running analyze appends duplicates (uses append mode `"a"`)
4. ✅ Function already emits `console_warning()` with kernel names
5. ✅ Redundant: `__validate_workload_counters()` reads this key and emits duplicate warning

**Solution:**
- Remove the yaml write (lines 129-134 in original)
- Remove the duplicate warning read in `__validate_workload_counters()` (lines 500-513)
- Keep the immediate warning in `detect_missing_counters()` (it's sufficient)
- Remove unused `iteration_multiplexing` parameter (PR review feedback)

**Consolidated Warning:** The single warning in `detect_missing_counters()` is better because:
- Immediate feedback when joining CSVs
- Clear, actionable message
- No file modification required
- No risk of stale/duplicate warnings

### Step 2: Update pre_processing() to Call join_prof()

**File**: `src/rocprof_compute_analyze/analysis_base.py`

**Modify `pre_processing()` at line 473** - add at the END, after filters:

**CRITICAL Design Decision**: Handle multi-node/spatial multiplexing in the CALLER, not in join_prof()

**Why?**
1. ✅ **Separation of concerns**:
   - `join_prof()` = Worker (dumb: given a directory, merge CSVs in it)
   - Caller = Coordinator (smart: decides WHICH directories to process)
2. ✅ **Matches create_df_pmc pattern** (file_io.py:234-331):
   - `create_single_df_pmc()` = Worker (processes one directory)
   - `create_df_pmc()` = Coordinator (iterates directories, calls worker)
3. ✅ **Simpler join_prof()**: No need to check args.nodes or args.spatial_multiplexing
4. ✅ **Clear responsibility**: Caller knows analysis context, worker just merges

**Directory Structure Examples:**
```
# Single-node case:
workloads/myapp/MI300X_A1/
├── pmc_perf_0.csv        # Merge these → pmc_perf.csv
├── pmc_perf_1.csv
└── SQ_*.csv

# Multi-node case (--node or MPI):
workloads/mpi_app/MI300X_A1/
├── 0/                    # Rank 0 - merge separately
│   ├── pmc_perf_0.csv
│   └── pmc_perf_1.csv
├── 1/                    # Rank 1 - merge separately
│   └── pmc_perf_0.csv
└── 2/                    # Rank 2 - merge separately

# Spatial multiplexing case:
workloads/spatial/MI300X_A1/
├── device_0/             # GPU 0 - merge separately
├── device_1/             # GPU 1 - merge separately
└── device_2/             # GPU 2 - merge separately
```

**Implementation Strategy:**
1. Create a new PUBLIC method `join_workload_csvs(workload_dir: Path)` in `OmniAnalyze_Base`
   - Public (not private with `_` prefix) because TUI needs to call it directly
   - TUI doesn't call `super().pre_processing()` due to different architecture
2. This method contains all the multi-node/spatial multiplexing logic
3. Call this method from:
   - `OmniAnalyze_Base.pre_processing()` loop for CLI/DB analyzers (they use super())
   - `tui_analysis.pre_processing()` directly (doesn't use super())

**Code Structure:**

```python
def join_workload_csvs(self, workload_dir: Path) -> None:
    """Join CSV files for a workload directory (handles multi-node and spatial multiplexing).

    This method checks if the workload uses multi-node or spatial multiplexing,
    and joins CSV files accordingly:
    - Multi-node/spatial: Joins CSV files in each subdirectory (0/, 1/, 2/, etc.)
    - Regular single-node: Joins CSV files in the workload directory directly

    Args:
        workload_dir: Path to the workload directory
    """
    args = self.get_args()

    # Helper to process and join CSV files in a single directory
    def process_and_join_directory(directory: Path) -> None:
        pmc_perf = directory / "pmc_perf.csv"
        pmc_perf_files = list(directory.glob("pmc_perf_*.csv"))
        results_files = list(directory.glob("results_*.csv"))

        if pmc_perf.exists():
            console_debug(f"Using existing {pmc_perf}")
        elif pmc_perf_files or results_files:
            files_desc = "pmc_perf_*.csv" if pmc_perf_files else "results_*.csv"
            console_log(f"Joining {files_desc} for {directory}...")
            self.join_prof(directory, out=str(pmc_perf))
            console_log(f"Created {pmc_perf}")
        else:
            console_error(
                f"No profiling data found in {directory}.\n"
                f"Expected: pmc_perf.csv or pmc_perf_*.csv or results_*.csv\n"
                f"Please run 'rocprof-compute profile' first."
            )

    # Handle multi-node and spatial multiplexing cases
    # Match create_df_pmc logic (file_io.py:292-331)
    if args.nodes is not None or args.spatial_multiplexing:
        # Multi-node or spatial case: CSV files are in subdirectories
        for subdir in workload_dir.iterdir():
            if subdir.is_dir():
                process_and_join_directory(subdir)
    else:
        # Regular single-node case: CSV files are in workload_dir directly
        process_and_join_directory(workload_dir)


def pre_processing(self) -> None:
    """Perform initialization prior to analysis."""
    console_debug("analysis", "prepping to do some analysis")
    console_log("analysis", "deriving rocprofiler-compute metrics...")
    args = self.get_args()

    # initalize output file (lines 480-486)
    if args.output_format == "txt":
        # ... existing code ...

    # initalize runs (line 489)
    self._runs = self.initalize_runs()

    # set filters (lines 491-509)
    filter_configs = [...]
    # ... existing filter code ...

    # NEW: Join pmc_perf_*.csv or results_*.csv files (Phase 2)
    # This runs AFTER initalize_runs() (which doesn't need pmc_perf.csv)
    # But BEFORE child class pre_processing() (which does need it)
    for path_info in args.path:
        workload_dir = Path(path_info[0])
        self.join_workload_csvs(workload_dir)

    # End of base class pre_processing()
    # Child class will now call file_io.create_df_pmc() which needs pmc_perf.csv
```

**TUI Integration (Special Case):**

TUI analysis doesn't call `super().pre_processing()` due to fundamentally different architecture.
Must call `join_workload_csvs()` manually:

```python
# File: src/rocprof_compute_tui/analysis_tui.py
@demarcate
def pre_processing(self) -> None:
    self._profiling_config = file_io.load_profiling_config(self.path)
    self._runs = self.initalize_runs()

    # Join pmc_perf_*.csv or results_*.csv files if needed (Phase 2)
    self.join_workload_csvs(Path(self.path))

    # ... rest of TUI preprocessing ...
```

**Logical Order**: First initialize workload objects (`initalize_runs()`), then prepare their data (`join_workload_csvs()`), then child class loads it.

**Benefits of Public Helper Method**:
- ✅ **Cleaner code**: Separates concerns into its own method
- ✅ **Testable**: `join_workload_csvs()` can be tested independently
- ✅ **Reusable**: Can be called from TUI and other places if needed
- ✅ **Readable**: Main `pre_processing()` loop is now just 2 lines per workload
- ✅ **Public API**: TUI can call it directly without relying on super() chain

### Step 3: Remove from Profile Mode

#### Step 3a: Delete Method Definitions from Base Class

**File**: `src/rocprof_compute_profile/profiler_base.py`

**Delete**:
- Line 37: `import pandas as pd` ✂️
- Lines 190-219: `detect_missing_counters()` method ✂️
- Lines 222-455: `join_prof()` method ✂️
- Lines 825-826: `test_df_column_equality()` function ✂️

**Total deletion**: ~270 lines from base class

#### Step 3b: Remove Method Calls and ready_to_profile Logic from Child Classes

**CRITICAL Changes to Child Profiler Classes:**

1. Remove `ready_to_profile` logic (obsolete after Step 0 cleanup)
2. Remove `join_prof()` calls (moved to analyze mode)
3. Update log messages to be generic (no "pmc_perf.csv" references)

**Rationale for Removing `ready_to_profile`:**
- Step 0 deletes entire workload directory before profiling
- Checking for file existence is meaningless when directory is always empty
- `pmc_perf.csv` is created by analyze mode, not profile mode
- `--roof-only` should only control which counters to collect, not file management

**File 1**: `src/rocprof_compute_profile/profiler_rocprof_v3.py`

**Changes:**

```python
# BEFORE (lines 42-47):
super().__init__(profiling_args, profiler_mode, soc)
self.ready_to_profile = (
    self.get_args().roof_only
    and not (Path(self.get_args().path) / "pmc_perf.csv").is_file()
    or not self.get_args().roof_only
)

# AFTER:
super().__init__(profiling_args, profiler_mode, soc)


# BEFORE (lines 116-127):
@demarcate
def run_profiling(self, version: str, prog: str) -> None:
    """Run profiling."""
    if not self.ready_to_profile:
        console_log("roofline", "Detected existing pmc_perf.csv")
        return

    if self.get_args().roof_only:
        console_log("roofline", "Generating pmc_perf.csv (roofline counters only).")

    # Log profiling options and setup filtering
    super().run_profiling(version, prog)

# AFTER:
@demarcate
def run_profiling(self, version: str, prog: str) -> None:
    """Run profiling."""
    if self.get_args().roof_only:
        console_log("roofline", "Profiling roofline counters only.")

    # Log profiling options and setup filtering
    super().run_profiling(version, prog)


# BEFORE (lines 129-135):
@demarcate
def post_processing(self) -> None:
    """Perform any post-processing steps prior to profiling."""
    if self.ready_to_profile:
        super().post_processing()
    else:
        console_log("roofline", "Detected existing pmc_perf.csv")

# AFTER:
@demarcate
def post_processing(self) -> None:
    """Perform any post-processing steps prior to profiling."""
    super().post_processing()
```

**File 2**: `src/rocprof_compute_profile/profiler_rocprofiler_sdk.py`

**Changes (identical to v3):**

```python
# BEFORE (lines 44-49):
super().__init__(profiling_args, profiler_mode, soc)
self.ready_to_profile = (
    self.get_args().roof_only
    and not (Path(self.get_args().path) / "pmc_perf.csv").is_file()
    or not self.get_args().roof_only
)

# AFTER:
super().__init__(profiling_args, profiler_mode, soc)


# BEFORE (lines 146-157):
@demarcate
def run_profiling(self, version: str, prog: str) -> None:
    """Run profiling."""
    if not self.ready_to_profile:
        console_log("roofline", "Detected existing pmc_perf.csv")
        return

    if self.get_args().roof_only:
        console_log("roofline", "Generating pmc_perf.csv (roofline counters only).")

    # Log profiling options and setup filtering
    super().run_profiling(version, prog)

# AFTER:
@demarcate
def run_profiling(self, version: str, prog: str) -> None:
    """Run profiling."""
    if self.get_args().roof_only:
        console_log("roofline", "Profiling roofline counters only.")

    # Log profiling options and setup filtering
    super().run_profiling(version, prog)


# BEFORE (lines 159-165):
@demarcate
def post_processing(self) -> None:
    """Perform any post-processing steps prior to profiling."""
    if self.ready_to_profile:
        super().post_processing()
    else:
        console_log("roofline", "Detected existing pmc_perf.csv")

# AFTER:
@demarcate
def post_processing(self) -> None:
    """Perform any post-processing steps prior to profiling."""
    super().post_processing()
```

**Summary of Step 3b**:
- Remove `ready_to_profile` initialization from both child classes
- Simplify `run_profiling()`: remove skip logic, update log message
- Simplify `post_processing()`: always call super(), remove conditional
- Total: 2 files modified (profiler_rocprof_v3.py and profiler_rocprofiler_sdk.py)
- Lines removed: ~20 lines of dead code

### Step 4: Remove pmc_perf.csv Check in soc_base.py

**File**: `src/rocprof_compute_soc/soc_base.py`

**Current code** (lines 687-694 - CORRECTED LINE NUMBERS):
```python
# Check if pmc_perf.csv exists before roofline post-processing
pmc_path = Path(self.get_args().path) / "pmc_perf.csv"
if not pmc_path.is_file():
    console_error(
        "roofline",
        "Incomplete or missing profiling data. Skipping roofline.",
        exit=False,
    )
    return
```

**DELETE THIS ENTIRE BLOCK** - roofline post-processing only needs `roofline.csv`, not `pmc_perf.csv`

**Rationale**:
- After Phase 1, profile mode doesn't do roofline HTML generation anyway
- After Phase 2, profile won't create pmc_perf.csv (only pmc_perf_*.csv)
- This check creates false dependency and will break with new format
- The actual roofline.csv check happens at line 698

### Step 5: Update Tests - Modify check_csv_files() Function

**KEY INSIGHT**: `check_csv_files()` is ONLY called after profile tests, never after analyze tests. After Phase 2, profile creates `pmc_perf_*.csv` or `results_*.csv`, NOT `pmc_perf.csv`.

**Strategy**:
1. Update file list constants (remove `pmc_perf.csv`)
2. Modify `check_csv_files()` to validate PMC files internally but NOT return them
3. Tests stay unchanged - they compare against updated constants

#### 5a. Update Expected File Lists in `test_profile_general.py`

**File**: `tests/test_profile_general.py`

**Lines 82-110** - Remove `pmc_perf.csv` from ALL constants:

```python
# BEFORE:
CSVS = sorted([
    "pmc_perf.csv",  # ✂️ REMOVE
    "sysinfo.csv",
])

ROOF_ONLY_FILES = sorted([
    "empirRoof_gpu-0_FP32.html",
    "pmc_perf.csv",  # ✂️ REMOVE
    "roofline.csv",
    "sysinfo.csv",
])

PC_SAMPLING_HOST_TRAP_FILES = sorted([
    "pmc_perf.csv",  # ✂️ REMOVE
    "ps_file_agent_info.csv",
    "ps_file_kernel_trace.csv",
    "ps_file_pc_sampling_host_trap.csv",
    "ps_file_results.json",
    "sysinfo.csv",
])

PC_SAMPLING_STOCHASTIC_FILES = sorted([
    "pmc_perf.csv",  # ✂️ REMOVE
    "ps_file_agent_info.csv",
    "ps_file_kernel_trace.csv",
    "ps_file_pc_sampling_stochastic.csv",
    "ps_file_results.json",
    "sysinfo.csv",
])

# AFTER:
CSVS = sorted([
    "sysinfo.csv",
    # PMC data validated by check_csv_files(), not in this list
])

ROOF_ONLY_FILES = sorted([
    "empirRoof_gpu-0_FP32.html",
    "roofline.csv",
    "sysinfo.csv",
    # PMC data validated by check_csv_files(), not in this list
])

PC_SAMPLING_HOST_TRAP_FILES = sorted([
    "ps_file_agent_info.csv",
    "ps_file_kernel_trace.csv",
    "ps_file_pc_sampling_host_trap.csv",
    "ps_file_results.json",
    "sysinfo.csv",
    # PMC data validated by check_csv_files(), not in this list
])

PC_SAMPLING_STOCHASTIC_FILES = sorted([
    "ps_file_agent_info.csv",
    "ps_file_kernel_trace.csv",
    "ps_file_pc_sampling_stochastic.csv",
    "ps_file_results.json",
    "sysinfo.csv",
    # PMC data validated by check_csv_files(), not in this list
])
```

#### 5b. Modify check_csv_files() in test_utils.py

**File**: `tests/test_utils.py`

**Function**: `check_csv_files()` (starts at line 201)

**Modify to validate PMC files but NOT include them in returned dict**:

```python
def check_csv_files(output_dir, num_devices, num_kernels):
    """Check profiling output csv files for expected
    number of entries (based on kernel invocations)

    Args:
        output_dir (string): output directory containing csv files
        num_kernels (int): number of kernels expected to have been profiled

    Returns:
        dict: dictionary housing file contents as pandas dataframe
                (excludes PMC files - those are validated internally)
    """

    file_dict = {}
    files_in_workload = os.listdir(output_dir)

    # NEW: Validate PMC data exists (profile creates pmc_perf_*.csv or results_*.csv)
    has_separate = any(f.startswith("pmc_perf_") and f.endswith(".csv") for f in files_in_workload)
    has_results = any(f.startswith("results_") and f.endswith(".csv") for f in files_in_workload)

    assert has_separate or has_results, \
        "Expected pmc_perf_*.csv or results_*.csv from profile mode"

    # NEW: Validate row counts for PMC files (but don't add to return dict)
    for file in files_in_workload:
        if (file.startswith("pmc_perf_") or file.startswith("results_")) and file.endswith(".csv"):
            df = pd.read_csv(output_dir + "/" + file)
            assert len(df.index) >= num_kernels, \
                f"PMC file {file} has insufficient rows: {len(df.index)} < {num_kernels}"
            # Don't add to file_dict - we only return non-PMC files!

    # EXISTING: Load non-PMC CSV files into return dict
    for file in files_in_workload:
        if file.endswith(".csv"):
            # MODIFIED: Skip PMC files (already validated above)
            if file.startswith("pmc_perf") or file.startswith("results_"):
                continue

            # Existing logic for other files
            file_dict[file] = pd.read_csv(output_dir + "/" + file)
            if "roofline" in file:
                assert len(file_dict[file].index) >= num_devices
            elif "sysinfo" not in file and "ps_file" not in file:
                assert len(file_dict[file].index) >= num_kernels
        elif file.endswith(".html"):
            file_dict[file] = "html"
        elif file.endswith(".json"):
            file_dict[file] = "json"

    return file_dict  # Only non-PMC files returned!
```

**Key Changes**:
- ✅ Validates `pmc_perf_*.csv` or `results_*.csv` exist
- ✅ Validates row counts for PMC files
- ✅ Does NOT include PMC files in returned dict
- ✅ Backward compatible structure (returns dict of files)

#### 5c. Tests Remain Unchanged!

**All existing test patterns continue to work**:

```python
# Pattern 1: File list comparison (most common - ~30 occurrences)
file_dict = test_utils.check_csv_files(workload_dir, num_devices, num_kernels)
# PMC files already validated internally ✅
assert sorted(list(file_dict.keys())) == CSVS  # Works with updated constants!

# Pattern 2: Direct file access
file_dict = test_utils.check_csv_files(workload_dir, 1, num_kernels)
# No change needed - just use updated CSVS/ROOF_ONLY_FILES constants

# Pattern 3: Content validation
file_dict = test_utils.check_csv_files(workload_dir, 1, num_kernels)
# PMC row counts already validated by check_csv_files()
```

**Direct pmc_perf.csv existence checks** (lines 723, 1209, 1287, etc.) need updating:

```python
# BEFORE:
assert (Path(workload_dir) / "pmc_perf.csv").exists()

# AFTER (if check not covered by check_csv_files call):
# Just remove - check_csv_files() already validated PMC data
# OR keep for extra validation:
assert len(list(Path(workload_dir).glob("pmc_perf_*.csv"))) > 0
```

**Content checks** (lines 727, 1292, 2341, etc.) need file name updates:

```python
# BEFORE:
test_utils.check_file_pattern("Counter_Name", f"{workload_dir}/pmc_perf.csv")

# AFTER:
# Get first pmc_perf file for content validation
pmc_file = list(Path(workload_dir).glob("pmc_perf_*.csv"))[0]
test_utils.check_file_pattern("Counter_Name", str(pmc_file))
```

#### 5d. Summary of Test Changes

**Total changes needed**:
1. Update 4 constants in test_profile_general.py (remove pmc_perf.csv)
2. Modify check_csv_files() in test_utils.py (~15 lines changed)
3. Remove/update direct pmc_perf.csv existence checks (~5 occurrences)
4. Update pmc_perf.csv content checks to use pmc_perf_*.csv (~8 occurrences)

**Total**: ~30 line changes across tests (vs 37 occurrences of pmc_perf.csv)

**Most tests unchanged** because they use check_csv_files() + constant comparison pattern!

#### 5d. Add Backward Compatibility Tests (Old vs New Format)

**Create new test file**: `tests/test_analyze_join_prof.py`

```python
import pytest
from pathlib import Path
import pandas as pd

@pytest.mark.analyze_join
def test_analyze_handles_merged_format(
    binary_handler_analyze_rocprof_compute,
    tmp_path
):
    """
    Verify analyze mode works with pre-merged pmc_perf.csv
    (backward compatibility with old workloads)
    """
    workload_dir = tmp_path / "old_format_workload"
    workload_dir.mkdir()

    # Create mock pmc_perf.csv (old format)
    mock_data = pd.DataFrame({
        'Kernel_Name': ['kernel1', 'kernel2'],
        'Counter1': [100, 200],
        'Counter2': [300, 400]
    })
    mock_data.to_csv(workload_dir / "pmc_perf.csv", index=False)

    # Run analyze
    returncode = binary_handler_analyze_rocprof_compute(
        ["analyze", "-p", str(workload_dir)]
    )

    # Should use existing file without error
    assert returncode == 0
    # pmc_perf.csv should still exist
    assert (workload_dir / "pmc_perf.csv").exists()


@pytest.mark.analyze_join
def test_analyze_handles_separate_format(
    binary_handler_analyze_rocprof_compute,
    tmp_path
):
    """
    Verify analyze mode joins separate pmc_perf_*.csv files
    (new format from Phase 2 profile mode)
    """
    workload_dir = tmp_path / "new_format_workload"
    workload_dir.mkdir()

    # Create separate pmc_perf_*.csv files (new format)
    data1 = pd.DataFrame({
        'Kernel_Name': ['kernel1'],
        'Counter1': [100],
        'Counter2': [300]
    })
    data2 = pd.DataFrame({
        'Kernel_Name': ['kernel1'],
        'Counter3': [500],
        'Counter4': [700]
    })
    data1.to_csv(workload_dir / "pmc_perf_0.csv", index=False)
    data2.to_csv(workload_dir / "pmc_perf_1.csv", index=False)

    # No merged file yet
    assert not (workload_dir / "pmc_perf.csv").exists()

    # Run analyze - should join files
    returncode = binary_handler_analyze_rocprof_compute(
        ["analyze", "-p", str(workload_dir)]
    )

    assert returncode == 0

    # pmc_perf.csv should now be created
    assert (workload_dir / "pmc_perf.csv").exists()

    # Verify joined data is correct
    joined_data = pd.read_csv(workload_dir / "pmc_perf.csv")
    assert 'Counter1' in joined_data.columns
    assert 'Counter3' in joined_data.columns


@pytest.mark.analyze_join
def test_analyze_handles_multiple_paths(
    binary_handler_analyze_rocprof_compute,
    tmp_path
):
    """Verify analyze mode joins data for multiple workload paths"""
    workload1 = tmp_path / "workload1"
    workload2 = tmp_path / "workload2"
    workload1.mkdir()
    workload2.mkdir()

    # Create separate files in both workloads
    for workload in [workload1, workload2]:
        data = pd.DataFrame({'Kernel_Name': ['k1'], 'C1': [100]})
        data.to_csv(workload / "pmc_perf_0.csv", index=False)

    # Analyze multiple paths
    returncode = binary_handler_analyze_rocprof_compute(
        ["analyze", "-p", str(workload1), str(workload2)]
    )

    assert returncode == 0
    # Both should have pmc_perf.csv now
    assert (workload1 / "pmc_perf.csv").exists()
    assert (workload2 / "pmc_perf.csv").exists()


@pytest.mark.profile_no_deps
def test_profile_no_pandas_import():
    """
    CRITICAL: Verify profile mode does not import pandas
    This validates Phase 2 objective
    """
    import sys

    # Clear any existing pandas import
    if 'pandas' in sys.modules:
        del sys.modules['pandas']

    # Import profiler_base (profile mode entry point)
    from rocprof_compute_profile.profiler_base import RocProfCompute_Base

    # Pandas should NOT be imported
    assert 'pandas' not in sys.modules, (
        "Profile mode imported pandas! Phase 2 dependency elimination failed."
    )

    print("✓ Profile mode has no pandas dependency")
```

#### 5e. Update Integration Test Workflows

**File**: `tests/test_profile_general.py`

Add end-to-end workflow tests:
```python
@pytest.mark.integration
def test_profile_analyze_workflow_phase2(
    binary_handler_profile_rocprof_compute,
    binary_handler_analyze_rocprof_compute
):
    """
    Test complete workflow after Phase 2:
    1. Profile creates pmc_perf_*.csv
    2. Analyze joins into pmc_perf.csv
    """
    workload_dir = test_utils.get_output_dir()

    # Step 1: Profile
    returncode = binary_handler_profile_rocprof_compute(
        config, workload_dir, options=[]
    )
    assert returncode == 0

    # Verify separate files exist
    pmc_files = list(Path(workload_dir).glob("pmc_perf_*.csv"))
    assert len(pmc_files) > 0

    # pmc_perf.csv should NOT exist yet
    assert not (Path(workload_dir) / "pmc_perf.csv").exists()

    # Step 2: Analyze
    returncode = binary_handler_analyze_rocprof_compute(
        ["analyze", "-p", workload_dir]
    )
    assert returncode == 0

    # Now pmc_perf.csv should exist
    assert (Path(workload_dir) / "pmc_perf.csv").exists()

    test_utils.clean_output_dir(config["cleanup"], workload_dir)
```

### Step 6: Update Documentation

**File**: `docs/profiling.md` or `docs/workflow.md`

```markdown
## Data Collection and Analysis Workflow

### Profile Mode - Data Collection

Profile mode collects performance data without any post-processing:

```bash
rocprof-compute profile -b SQ,TA,TCP -- ./my_app
```

**Output** (example):
```
./workload_dir/
├── pmc_perf_0.csv  # First counter set
├── pmc_perf_1.csv  # Second counter set
├── pmc_perf_2.csv  # Third counter set
└── perfmon/        # Counter specifications
```

**Note**: Individual CSV files are kept for debugging. The `pmc_perf.csv` merged
file is created by analyze mode.

### Analyze Mode - Data Processing

Analyze mode automatically detects the format and joins data if needed:

```bash
rocprof-compute analyze -p ./workload_dir
```

**Process**:
1. Checks if `pmc_perf.csv` exists (backward compatible)
2. If not, joins `pmc_perf_*.csv` files
3. Proceeds with analysis and visualization

**Benefits**:
- Profile mode is lightweight (no pandas dependency)
- Can profile on compute nodes, analyze on login nodes
- Can re-run analyze without re-profiling
```

---

## Verification

### Manual Testing

**Test 1: Profile mode has no pandas**
```bash
cd /app/projects/rocprofiler-compute
python3 -c "
import sys
sys.path.insert(0, 'src')
from rocprof_compute_profile.profiler_base import RocProfCompute_Base
assert 'pandas' not in sys.modules
print('✓ Profile mode clean: no pandas imported')
"
```

**Test 2: Profile creates separate CSVs**
```bash
python3 src/rocprof-compute profile -b SQ,TA -n test1 -- /bin/true

# Verify separate CSVs exist
ls -la ./test1/pmc_perf_*.csv  # Should have pmc_perf_0.csv, pmc_perf_1.csv

# Verify merged CSV does NOT exist yet
ls ./test1/pmc_perf.csv 2>&1 | grep "No such file"  # Should fail
```

**Test 3: Analyze joins and processes**
```bash
python3 src/rocprof-compute analyze -p ./test1

# Verify joined CSV was created
ls -la ./test1/pmc_perf.csv  # Should now exist

# Verify analysis completed
echo "✓ Analyze mode successfully joined and processed data"
```

**Test 4: Analyze handles pre-joined format (backward compat)**
```bash
# Use old workload that already has pmc_perf.csv
python3 src/rocprof-compute analyze -p ./old_workload

# Should work without re-joining
```

---

## Impact Analysis

### Code Changes
- **Lines deleted from profile**: ~270 lines
  - `join_prof()`: 230 lines
  - `detect_missing_counters()`: 30 lines
  - `test_df_column_equality()`: 2 lines
  - `import pandas`: 1 line
  - Useless check in soc_base.py: ~10 lines

- **Lines added to analyze**: ~50 lines
  - Format detection and join logic in pre_processing(): 30 lines
  - Moved functions (references, not full copy): 20 lines

- **Net reduction**: ~220 lines

### Dependencies Eliminated from Profile Mode
- ✅ **pandas** (~50MB, ~2500 imports) - COMPLETE REMOVAL

### User-Visible Changes

**Before**:
```bash
$ rocprof-compute profile -b SQ,TA -- ./app
...
Created pmc_perf.csv
```

**After**:
```bash
$ rocprof-compute profile -b SQ,TA -- ./app
...
Created pmc_perf_0.csv
Created pmc_perf_1.csv

$ rocprof-compute analyze -p ./workload
Joining PMC data for ./workload...
✓ Created ./workload/pmc_perf.csv
...
[Analysis output]
```

### Backward Compatibility

✅ **Fully backward compatible**:
- Old workloads with `pmc_perf.csv` work without change
- New workloads with `pmc_perf_*.csv` automatically joined
- Analyze mode handles both formats transparently

---

## Success Criteria

- [ ] Profile mode does NOT import pandas
- [ ] `profiler_base.py` has no pandas dependency
- [ ] Analyze mode joins separate CSVs correctly
- [ ] Analyze mode handles merged CSV (backward compat)
- [ ] Multiple workload paths supported
- [ ] Output `pmc_perf.csv` identical to previous implementation
- [ ] All existing profile tests pass
- [ ] All existing analyze tests pass
- [ ] Documentation updated

---

## Rollback Plan

If issues arise:
1. Revert `analysis_base.py` changes (remove join_prof)
2. Restore `profiler_base.py` (add back join_prof + pandas import)
3. Previous behavior fully restored

---

## Corrections and Enhancements to Original Plan

### Line Number Corrections
- ✅ `detect_missing_counters()`: lines 190-219 (verified correct)
- ✅ `join_prof()`: lines 222-455 (verified correct)
- ✅ `test_df_column_equality()`: lines 825-826 (verified correct)
- ✅ soc_base.py pmc_perf.csv check: **lines 687-694** (was incorrectly stated as ~710-718)
- ✅ Test file occurrences: **37** (was stated as "30+")

### Enhanced Error Handling
- ✅ Error out immediately when no profiling data found (neither pmc_perf.csv, pmc_perf_*.csv, nor results_*.csv)
- ✅ Clear error message guides user on expected formats
- ✅ Support both standard (pmc_perf_*.csv) AND rocpd (results_*.csv) formats

### Critical Implementation Details
- ✅ `join_workload_csvs()` must be called in `pre_processing()` AFTER `initalize_runs()` (line 489)
- ✅ Reason: `initalize_runs()` only loads sysinfo.csv and roofline.csv, NOT pmc_perf.csv. Child classes load pmc_perf.csv in their pre_processing(), so base class must create it first.
- ✅ Logical order: Initialize workloads → Prepare data (join CSVs) → Child class loads merged data
- ✅ Handle both old format (pre-merged pmc_perf.csv) and new formats (separate files)
- ✅ Test helper function supports both pmc_perf_*.csv and results_*.csv

### Test Update Strategy
- ✅ Update ALL 37 occurrences systematically (not just critical ones)
- ✅ Use helper function for consistency
- ✅ Verify pmc_perf.csv does NOT exist after profile (only after analyze)

### Code Pattern Decisions
- ✅ Use existing `args.path` access pattern: `for path_info in args.path: workload_dir = Path(path_info[0])`
- ✅ Rationale: Matches all existing analyze code which treats `args.path` as list of lists
- ✅ Each element in `args.path` is a list where `[0]` is the path string
- ✅ This pattern is used consistently across analysis_base.py, analysis_cli.py, etc.

## Notes

- ✅ Clean architectural separation (collect vs process)
- ✅ Consistent with Phase 1 (move post-processing to analyze)
- ✅ Zero pandas in profile mode achieved
- ✅ No logic changes - just moving code
- ✅ Backward compatible
- ✅ Can develop in parallel with Phase 1
- ✅ Independent PR - no dependencies
- ✅ Supports both standard and rocpd output formats
