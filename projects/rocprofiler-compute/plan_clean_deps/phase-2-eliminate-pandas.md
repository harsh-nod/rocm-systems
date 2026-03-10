# Phase 2: Move join_prof() to Analyze Mode - Eliminate Pandas from Profile

**PR #2** | **Theme**: Move CSV joining to analyze mode
**Objective**: Remove pandas dependency from profile code path by moving join_prof() to analyze
**Dependencies**: None (independent PR, can develop in parallel with Phase 1)
**Duration**: 2-3 days
**Status**: Ready to implement

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
- Refactoring join_prof() logic (keep as-is, just move it)
- Simplifying concat/merge operations (future optimization)
- Utils refactoring (Phase 4)
- YAML elimination (Phase 3)

---

## Implementation Plan

### Step 1: Move join_prof() Functions to Analyze

**File**: `src/rocprof_compute_analyze/analysis_base.py`

**Add to class** (likely in `pre_processing()` or new method):

```python
def join_prof(self, workload_dir: Path, out: Optional[str] = None) -> Optional[pd.DataFrame]:
    """
    Join separated rocprof runs into single pmc_perf.csv.

    Moved from profiler_base.py to eliminate pandas from profile mode.
    This is fundamentally a data preparation step for analysis.
    """
    # Copy entire join_prof() implementation from profiler_base.py (lines 222-455)
    # Keep logic EXACTLY as-is - no refactoring
    # ... (230 lines)

def detect_missing_counters(self, df: pd.DataFrame, workload_dir: Path) -> None:
    """Detect missing counter values in joined dataframe"""
    # Copy from profiler_base.py (lines 190-219)
    # ... (30 lines)

def test_df_column_equality(df: pd.DataFrame) -> bool:
    """Test if all columns in dataframe are equal"""
    # Copy from profiler_base.py (lines 825-826)
    return df.eq(df.iloc[:, 0], axis=0).all(1).all()
```

### Step 2: Update pre_processing() to Call join_prof()

**File**: `src/rocprof_compute_analyze/analysis_base.py`

**Modify `pre_processing()`**:

```python
def pre_processing(self) -> None:
    """Load and prepare profiling data for analysis"""

    args = self.get_args()

    # Handle multiple workload paths
    for path_list in args.path:
        for workload_path in path_list:
            workload_dir = Path(workload_path)

            # Check format: merged vs separate CSVs
            pmc_perf = workload_dir / "pmc_perf.csv"
            pmc_perf_files = list(workload_dir.glob("pmc_perf_*.csv"))

            if not pmc_perf.exists() and pmc_perf_files:
                # New format: separate CSVs need joining
                console_log(f"Joining PMC data for {workload_dir}...")
                self.join_prof(workload_dir, out=str(pmc_perf))
                console_log(f"✓ Created {pmc_perf}")
            elif pmc_perf.exists():
                # Old format or already joined
                console_debug(f"Using existing {pmc_perf}")
            else:
                console_error(
                    f"No PMC data found in {workload_dir}. "
                    f"Expected pmc_perf.csv or pmc_perf_*.csv files."
                )

            # Continue with existing pre_processing logic...
            # Load raw_pmc from pmc_perf.csv
            # Apply multiplexing if needed
            # etc.
```

### Step 3: Remove from Profile Mode

**File**: `src/rocprof_compute_profile/profiler_base.py`

**Delete**:
- Line 37: `import pandas as pd` ✂️
- Lines 190-219: `detect_missing_counters()` method ✂️
- Lines 222-455: `join_prof()` method ✂️
- Lines 825-826: `test_df_column_equality()` function ✂️
- Any calls to `join_prof()` in `post_processing()` ✂️

**Total deletion**: ~270 lines

### Step 4: Remove Useless Check in soc_base.py

**File**: `src/rocprof_compute_soc/soc_base.py`

**Current code** (lines ~710-718 or similar):
```python
# Check if pmc_perf.csv exists before roofline post-processing
if not (args.path / "pmc_perf.csv").exists():
    console_error("roofline", "pmc_perf.csv not found", exit=False)
    return
```

**Remove this check** - roofline post-processing only needs `roofline.csv`, not `pmc_perf.csv`

After Phase 1, profile mode doesn't do roofline post-processing anyway, so this is dead code.

### Step 5: Update Tests - CRITICAL Changes Required

**BREAKING CHANGE**: ALL tests expect `pmc_perf.csv` after profile mode. After Phase 2, profile creates `pmc_perf_*.csv` instead.

#### 5a. Update Expected File Lists in `test_profile_general.py`

**File**: `tests/test_profile_general.py`

**Lines 82-109** - Multiple file list constants expect `pmc_perf.csv`:
```python
# CURRENT (WRONG after Phase 2):
CSVS = sorted([
    "pmc_perf.csv",  # ❌ No longer created by profile!
    "sysinfo.csv",
])

ROOF_ONLY_FILES = sorted([
    "roofline.csv",
    "pmc_perf.csv",  # ❌ Wrong!
    "sysinfo.csv",
])

PC_SAMPLING_HOST_TRAP_FILES = sorted([
    "pmc_perf.csv",  # ❌ Wrong!
    "ps_file_agent_info.csv",
    ...
])

# UPDATED:
CSVS = sorted([
    "sysinfo.csv",
    # pmc_perf.csv no longer created by profile
    # Instead: pmc_perf_*.csv files exist
])

ROOF_ONLY_FILES = sorted([
    "roofline.csv",
    "sysinfo.csv",
])

PC_SAMPLING_HOST_TRAP_FILES = sorted([
    "ps_file_agent_info.csv",
    "ps_file_kernel_trace.csv",
    ...
])
```

**Add new helper** to check for pmc_perf_*.csv pattern:
```python
def check_pmc_perf_separate_files(workload_dir):
    """Verify profile created separate pmc_perf_*.csv files"""
    pmc_files = list(Path(workload_dir).glob("pmc_perf_*.csv"))
    assert len(pmc_files) > 0, "Profile should create pmc_perf_*.csv files"
    return pmc_files
```

#### 5b. Update ALL Profile Tests That Check pmc_perf.csv (30+ occurrences!)

**Search all occurrences**:
```bash
grep -n "pmc_perf\.csv" tests/test_profile_general.py
# Returns 30+ lines!
```

**Pattern for updates**:
```python
# BEFORE (BROKEN after Phase 2):
def test_something(binary_handler_profile_rocprof_compute):
    returncode = binary_handler_profile_rocprof_compute(...)
    assert (Path(workload_dir) / "pmc_perf.csv").exists()  # ❌ FAILS!

# AFTER (CORRECT):
def test_something(binary_handler_profile_rocprof_compute):
    returncode = binary_handler_profile_rocprof_compute(...)
    # Profile creates separate files
    pmc_files = list(Path(workload_dir).glob("pmc_perf_*.csv"))
    assert len(pmc_files) > 0, "pmc_perf_*.csv files should exist"
```

**Specific lines to fix in `test_profile_general.py`**:
- Line 723: `assert (Path(workload_dir) / "pmc_perf.csv").exists()`
- Line 727: `test_utils.check_file_pattern("Counter_Name", f"{workload_dir}/pmc_perf.csv")`
- Line 1209: `assert os.path.exists(f"{workload_dir}/pmc_perf.csv")`
- And ~27 more occurrences in file list checks

#### 5c. Update test_utils.py File Checking Logic

**File**: `tests/test_utils.py` (264KB file)

**Search for pmc_perf.csv checks**:
```python
# Likely in check_csv_files() or similar helpers
def check_csv_files(workload_dir, num_devices, num_kernels):
    # BEFORE:
    expected_files = ["pmc_perf.csv", "sysinfo.csv", ...]  # ❌

    # AFTER:
    # Check for separate pmc_perf files
    pmc_files = list(Path(workload_dir).glob("pmc_perf_*.csv"))
    assert len(pmc_files) > 0
    expected_files = ["sysinfo.csv", ...]  # ✅
```

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

## Notes

- ✅ Clean architectural separation (collect vs process)
- ✅ Consistent with Phase 1 (move post-processing to analyze)
- ✅ Zero pandas in profile mode achieved
- ✅ No logic changes - just moving code
- ✅ Backward compatible
- ✅ Can develop in parallel with Phase 1
- ✅ Independent PR - no dependencies
