# Phase 1: Move ALL Roofline Post-Processing to Analyze Mode

**PR #1** | **Theme**: Complete separation of data collection vs analysis
**Objective**: Profile mode only runs microbenchmarks; all AI calculation and visualization in analyze mode
**Dependencies**: None (independent PR)
**Duration**: 2-3 days
**Status**: READY TO IMPLEMENT - Low-hanging fruit
**JIRA Tickets**: AIPROFCOMP-107, AIPROFCOMP-29

---

## Problem Statement

Currently, profile mode does TOO MUCH roofline processing:
1. ✓ Runs microbenchmarks (needed)
2. ✓ Creates `roofline.csv` (needed)
3. ✗ Loads PMC data and applies multiplexing (should be in analyze)
4. ✗ Calculates arithmetic intensity (should be in analyze)
5. ✗ Generates HTML visualization (should be in analyze)

This creates unnecessary dependencies on pandas, plotly, and other heavy packages in profile mode.

**Current code location**: `src/rocprof_compute_soc/soc_base.py` lines 700-757

---

## Objective

**Clean separation of concerns**:
- **Profile mode**: Microbenchmark execution → `roofline.csv` only
- **Analyze mode**: Load data → Calculate AI → Generate visualizations (ASCII + HTML)

**Remove from profile path**:
- plotly, dash, dash-bootstrap-components, dash-svg
- plotext, plotille
- PMC data loading (depends on pandas currently)
- Arithmetic intensity calculation

---

## Scope

### In Scope

**Profile Mode Changes** (`soc_base.py`):
- Keep microbenchmark execution (generates `roofline.csv`)
- Remove all PMC data loading (lines 732-754)
- Remove multiplexing operations (lines 744-751)
- Remove `roofline_obj.post_processing()` call (line 756)
- Remove deprecation warning (lines 720-729) - we're doing it now!
- Add helpful message about running analyze for visualizations

**Analyze Mode Changes** (`roofline.py`):
- Modify `cli_generate_plot()` to also create HTML
- Call existing `generate_plot()` method for HTML generation
- Save HTML to workload directory alongside ASCII output
- Add INFO log that HTML was created

**Tests**:
- Verify profile creates only `roofline.csv`, no HTML
- Verify analyze creates both ASCII and HTML charts
- Validate HTML contains same data as ASCII

**Documentation**:
- Update profiling workflow docs
- Document two-step process (profile → analyze)

### Out of Scope
- Pandas elimination (Phase 2)
- YAML handling (Phase 3)
- WebUI mode changes (already generates HTML)
- Utils refactoring (Phase 4)

---

## Implementation Plan

### Step 1: Simplify Profile Mode's `post_profiling()`

**File**: `src/rocprof_compute_soc/soc_base.py`

**Current code** (lines 670-757):
```python
def post_profiling(self, args: argparse.Namespace, output_dir: Path) -> None:
    """Perform post-profiling roofline operations"""

    if not args.roof_only and not args.filter_blocks:
        return

    # ... microbenchmark execution (lines 680-718)
    # Creates roofline.csv

    # Validate roofline.csv
    is_valid, error_msg = validate_roofline_csv(self.get_args().path)

    # DEPRECATION WARNING (lines 720-729) - REMOVE THIS

    # REMOVE EVERYTHING BELOW (lines 731-757)
    # - Load PMC data
    # - Apply spatial/iteration multiplexing
    # - Call roofline_obj.post_processing()
```

**New simplified code**:
```python
def post_profiling(self, args: argparse.Namespace, output_dir: Path) -> None:
    """
    Perform post-profiling roofline operations.

    Profile mode: Only runs microbenchmarks to generate roofline.csv
    Analyze mode: Performs AI calculation and visualization
    """

    if not args.roof_only and not args.filter_blocks:
        return

    # Run microbenchmarks (keep existing code lines 680-708)
    try:
        from utils.benchmark import Benchmark

        benchmark = Benchmark(
            args=args,
            mspec=self._mspec,
            logger_name="roofline",
        )
        result = benchmark.run_on_devices([self.get_args().device])
        benchmark.dump_csv(result, f"{self.get_args().path}/roofline.csv")
    except Exception as e:
        console_error(
            "roofline",
            f"Benchmark execution failed: {e}. Skipping roofline.",
            exit=False,
        )
        return

    # Validate roofline.csv (keep existing code lines 710-718)
    is_valid, error_msg = validate_roofline_csv(self.get_args().path)
    if not is_valid:
        console_error(
            "roofline",
            f"Roofline post-processing skipped: {error_msg}",
            exit=False,
        )
        return

    # NEW: Inform user about analyze step for visualizations
    console_log(
        f"✓ Roofline benchmark data saved to {self.get_args().path}/roofline.csv\n"
        f"  Run 'rocprof-compute analyze -p {self.get_args().path}' "
        f"to generate roofline charts"
    )

    # REMOVED: All PMC loading and post_processing() call
    # This now happens ONLY in analyze mode
```

**Lines to delete**: 720-757 (37 lines removed)

---

### Step 2: Refactor Roofline Functions - Eliminate `calc_ai_profile()` and Standardize API

**Objective**: Clean up roofline code to use single AI calculation method and consistent naming

**Files**: `src/roofline.py`, `src/utils/roofline_calc.py`

#### 2a. Eliminate `calc_ai_profile()` (roofline_calc.py)

**Current situation**:
- `calc_ai_profile()` - Uses hardcoded equations, called by `empirical_roofline()` in WebUI/standalone mode
- `calc_ai_analyze()` - Uses YAML-based metrics, called by `cli_generate_plot()` in CLI mode

**Problem**: Duplicate logic, inconsistent approaches

**Solution**: Delete `calc_ai_profile()` entirely, use only `calc_ai_analyze()` everywhere

**File**: `/app/projects/rocprofiler-compute/src/utils/roofline_calc.py`

**Action**: Delete `calc_ai_profile()` function (lines 474-827) - **353 lines deleted**

#### 2b. Rename `empirical_roofline()` → `html_generate_plot()`

**Current**:
- `empirical_roofline()` - Generates HTML plots for WebUI/standalone (lines 226-375)
- `cli_generate_plot()` - Generates ASCII plots for CLI (lines 1071-1323)

**Problem**: Inconsistent naming, unclear purpose

**Solution**: Rename for symmetry and clarity:
- `html_generate_plot()` - Generates HTML plots (WebUI + standalone)
- `cli_generate_plot()` - Generates ASCII plots (CLI)

**File**: `/app/projects/rocprofiler-compute/src/roofline.py`

**Action**: Rename function (line 226):
```python
# OLD
def empirical_roofline(self, ret_df: dict[str, pd.DataFrame]) -> Optional[html.Section]:

# NEW
def html_generate_plot(self, workload: schema.Workload, ai_data: dict, config: dict, arch_config: schema.ArchConfig) -> Optional[html.Section]:
```

#### 2c. Move AI Calculation Outside Plot Functions

**Current flow**:
```python
# WebUI mode
empirical_roofline(ret_df):
    ai_data = calc_ai_profile(...)  # Inside function
    generate_plot(dtype, ai_data)   # Create HTML

# CLI mode
cli_generate_plot(workload, ...):
    ai_data = calc_ai_analyze(...)  # Inside function
    plt.plot(...)                    # Create ASCII
```

**New flow** (AI calculation moved to callers):
```python
# WebUI mode (analysis_webui.py)
ai_data = calc_ai_analyze(workload, ...)  # OUTSIDE
html_plot = html_generate_plot(workload, ai_data, ...)  # Use pre-calculated AI

# CLI mode (analysis_cli.py)
ai_data = calc_ai_analyze(workload, ...)  # OUTSIDE
if standalone_mode:
    html_plot = html_generate_plot(workload, ai_data, ...)  # HTML for standalone
ascii_plot = cli_generate_plot(workload, ai_data, ...)  # ASCII for terminal
```

**Benefits**:
- AI calculated once, used by both HTML and ASCII
- Clean separation: calculation vs visualization
- Easier to test independently

#### 2d. Updated Function Signatures

**`html_generate_plot()`** (formerly `empirical_roofline()`):
```python
def html_generate_plot(
    self,
    workload: schema.Workload,      # NEW: use workload not ret_df
    ai_data: dict,                  # NEW: pre-calculated AI data
    config: dict,                   # NEW: profiling config
    arch_config: schema.ArchConfig, # NEW: architecture config
    is_standalone: bool = False,    # Keep: standalone vs WebUI mode
) -> Optional[html.Section]:
    """
    Generate HTML roofline plot using Plotly.

    Args:
        workload: Workload object with path and data
        ai_data: Pre-calculated arithmetic intensity data from calc_ai_analyze()
        config: Profiling configuration dict
        arch_config: Architecture configuration
        is_standalone: If True, save HTML files; if False, return html.Section for WebUI

    Returns:
        html.Section for WebUI mode, None for standalone mode (files saved)
    """
    # Store AI data (no longer calculates it)
    self.__ai_data = ai_data

    # Build ceiling data from roofline.csv
    self.__ceiling_data = construct_roof(...)

    # Generate Plotly figure(s)
    for dtype in datatypes:
        figure = self.generate_plot(dtype=dtype, ...)

        if is_standalone:
            # Save HTML file
            figure.write_html(f"{workload.path}/empirRoof_gpu-{dev_id}_{dtype}.html")
        else:
            # Return html.Section for WebUI
            return html.Section(...)
```

**`cli_generate_plot()`** (updated):
```python
def cli_generate_plot(
    self,
    dtype: str,
    workload: schema.Workload,
    ai_data: dict,                  # NEW: pre-calculated AI data
    config: dict[str, Any],
    arch_config: schema.ArchConfig,
) -> Optional[str]:
    """
    Generate ASCII roofline plot for CLI using plotext.

    Args:
        dtype: Data type (FP32, FP64, etc.)
        workload: Workload object
        ai_data: Pre-calculated arithmetic intensity data from calc_ai_analyze()
        config: Profiling configuration
        arch_config: Architecture configuration

    Returns:
        ASCII plot string for terminal display
    """
    # Store AI data (no longer calculates it)
    self.__ai_data = ai_data

    # Build ceiling data from roofline.csv
    self.__ceiling_data = construct_roof(...)

    # Generate ASCII chart using plotext
    plt.plot(...)  # ... existing plotext code ...

    return plt.build()  # Return ASCII string
```

#### 2e. Update Callers

**File**: `/app/projects/rocprofiler-compute/src/rocprof_compute_analyze/analysis_webui.py`

**Change** (around line 240):
```python
# OLD
roof_obj.empirical_roofline(
    ret_df=parser.apply_filters(
        workload=base_data[base_run],
        dir_path=self.dest_dir,
        is_gui=True,
        debug=args.debug,
    )
)

# NEW
from utils.roofline_calc import calc_ai_analyze

# Calculate AI once
ai_data = calc_ai_analyze(
    workload=base_data[base_run],
    mspec=roof_obj.mspec,
    sort_type="kernels",  # or from config
    config=self._profiling_config,
    arch_config=arch_config,
)

# Generate HTML plot for WebUI
roof_obj.html_generate_plot(
    workload=base_data[base_run],
    ai_data=ai_data,
    config=self._profiling_config,
    arch_config=arch_config,
    is_standalone=False,  # WebUI mode
)
```

**File**: `/app/projects/rocprofiler-compute/src/rocprof_compute_analyze/analysis_cli.py`

**Change** (around lines 151-167):
```python
# OLD
roof_plot = roof_obj.cli_generate_plot(
    dtype=roof_obj.get_dtype()[0],
    workload=workload,
    config=self._profiling_config,
    arch_config=arch_config,
)

# NEW
from utils.roofline_calc import calc_ai_analyze

# Calculate AI once (used by both HTML and ASCII)
ai_data = calc_ai_analyze(
    workload=workload,
    mspec=roof_obj.mspec,
    sort_type=roof_obj.get_sort_type(),
    config=self._profiling_config,
    arch_config=arch_config,
)

# Generate HTML plot if standalone mode or specific flag
# (In standalone mode, user ran profile --roof-only, now analyzing)
if should_generate_html:  # Logic TBD: check if roofline.csv exists
    roof_obj.html_generate_plot(
        workload=workload,
        ai_data=ai_data,
        config=self._profiling_config,
        arch_config=arch_config,
        is_standalone=True,  # Saves HTML files
    )

# Generate ASCII plot for CLI display
roof_plot = roof_obj.cli_generate_plot(
    dtype=roof_obj.get_dtype()[0],
    workload=workload,
    ai_data=ai_data,  # Pass pre-calculated AI
    config=self._profiling_config,
    arch_config=arch_config,
)
```

---

### Step 3: Update Tests - CRITICAL Changes Required

**BREAKING CHANGE**: Many existing tests expect roofline HTML after profile mode. ALL these tests will FAIL without updates.

#### 3a. Update Expected File Lists in `test_profile_general.py`

**File**: `tests/test_profile_general.py`

**Lines 87-92** - `ROOF_ONLY_FILES` constant:
```python
# CURRENT (WRONG after Phase 1):
ROOF_ONLY_FILES = sorted([
    "empirRoof_gpu-0_FP32.html",  # ❌ No longer generated by profile!
    "pmc_perf.csv",
    "roofline.csv",
    "sysinfo.csv",
])

# UPDATED:
ROOF_ONLY_FILES = sorted([
    "roofline.csv",  # ✅ Profile only creates CSV
    "sysinfo.csv",
    # pmc_perf.csv still created in Phase 1 (removed in Phase 2)
    "pmc_perf.csv",
])

# NEW constant for analyze mode validation:
ROOF_ANALYZE_HTML_FILES = [
    "empirRoof_gpu-0_FP32.html",  # Generated by analyze mode
]
```

#### 3b. Update All Roofline Tests (11 tests affected)

**Affected tests in `test_profile_general.py`**:
- `test_roof_basic_validation()` - line 1147
- `test_roof_multiple_data_types()` - line 1183
- `test_roof_invalid_data_type()` - line 1221
- `test_roof_file_validation()` - line 1250
- `test_roofline_kernel_filter_both()` - line ~1495
- `test_roofline_unsupported_datatype_error()` - line 1520
- Parametrized roofline tests - line 1545 onwards

**Pattern for ALL roofline profile tests** - Remove HTML expectations:
```python
# BEFORE (BROKEN):
def test_roof_basic_validation(binary_handler_profile_rocprof_compute):
    returncode = binary_handler_profile_rocprof_compute(
        config, workload_dir, options, check_success=False, roof=True
    )
    assert returncode == 0

    # This line FAILS after Phase 1:
    html_files = list(Path(workload_dir).glob("empirRoof_*.html"))
    assert len(html_files) > 0  # ❌ Profile doesn't generate HTML anymore!

# AFTER (CORRECT):
def test_roof_basic_validation(binary_handler_profile_rocprof_compute):
    returncode = binary_handler_profile_rocprof_compute(
        config, workload_dir, options, check_success=False
    )
    assert returncode == 0

    # Verify roofline CSV exists
    assert (Path(workload_dir) / "roofline.csv").exists()

    # Verify NO HTML generated by profile
    html_files = list(Path(workload_dir).glob("empirRoof_*.html"))
    assert len(html_files) == 0  # ✅ Correct expectation
```

**Remove all** `html_files = list(Path(workload_dir).glob("empirRoof_*.html"))` assertions from profile tests!

#### 3c. Remove or Update `roof=True` Parameter

**Lines with `roof=True`** (grep search finds ~6 occurrences):
- Line 1164, 1187, 1505, etc.

The `roof=True` parameter may trigger special roofline behavior in test helpers. After Phase 1:
- Option 1: Remove `roof=True` entirely
- Option 2: Make test helper ignore it (roofline HTML moved to analyze)

**Update test helper** (likely in `conftest.py`):
```python
# conftest.py - Update binary_handler_profile_rocprof_compute
def binary_handler_profile_rocprof_compute(..., roof=False):
    # REMOVE or ignore roof parameter
    # Profile no longer handles HTML generation
    ...
```

#### 3d. Add New Analyze-Mode Roofline Tests

**File**: `tests/test_analyze_commands.py`

Add comprehensive roofline HTML generation tests for analyze mode:

```python
@pytest.mark.roofline_analyze
def test_analyze_generates_roofline_html(
    binary_handler_profile_rocprof_compute,
    binary_handler_analyze_rocprof_compute
):
    """
    Test complete workflow: profile creates CSV, analyze creates HTML
    """
    workload_dir = test_utils.get_output_dir()

    # Step 1: Profile creates roofline.csv
    returncode = binary_handler_profile_rocprof_compute(
        config, workload_dir, ["--roof-only"]
    )
    assert returncode == 0
    assert (Path(workload_dir) / "roofline.csv").exists()

    # Step 2: Analyze generates HTML
    returncode = binary_handler_analyze_rocprof_compute(
        ["analyze", "-p", workload_dir]
    )
    assert returncode == 0

    # Validate HTML exists
    html_files = list(Path(workload_dir).glob("empirRoof_*.html"))
    assert len(html_files) > 0
    assert (Path(workload_dir) / "empirRoof_gpu-0_FP32.html").exists()

    test_utils.clean_output_dir(config["cleanup"], workload_dir)


@pytest.mark.roofline_analyze
def test_analyze_roofline_multiple_datatypes(
    binary_handler_profile_rocprof_compute,
    binary_handler_analyze_rocprof_compute
):
    """Test analyze generates correct HTML for multiple data types"""
    workload_dir = test_utils.get_output_dir()

    # Profile with multiple data types
    returncode = binary_handler_profile_rocprof_compute(
        config, workload_dir,
        ["--roof-only", "--roofline-data-type", "FP16,FP32"]
    )
    assert returncode == 0

    # Analyze generates multiple HTMLs
    returncode = binary_handler_analyze_rocprof_compute(
        ["analyze", "-p", workload_dir]
    )
    assert returncode == 0

    # Validate both HTML files exist
    assert (Path(workload_dir) / "empirRoof_gpu-0_FP16.html").exists()
    assert (Path(workload_dir) / "empirRoof_gpu-0_FP32.html").exists()

    test_utils.clean_output_dir(config["cleanup"], workload_dir)
```

---

### Step 4: Update Tests for Renamed Functions

**File**: `tests/test_roofline_calc_ai_analyze.py`

**Changes needed**:
1. Update any tests calling `empirical_roofline()` → `html_generate_plot()`
2. Update any tests calling `calc_ai_profile()` → `calc_ai_analyze()` (if any direct calls exist)
3. Update imports if roofline functions moved

#### Test 1: Profile Creates Only CSV
```python
def test_profile_roofline_creates_csv_only(
    binary_handler_profile_rocprof_compute,
    tmp_path
):
    """Verify profile mode creates roofline.csv but NOT HTML"""
    config = {'app_1': ['/bin/true']}
    workload_dir = tmp_path / "test_roofline_profile"
    workload_dir.mkdir(parents=True, exist_ok=True)

    # Run profile with roofline
    returncode = binary_handler_profile_rocprof_compute(
        config=config,
        workload_dir=str(workload_dir),
        options=['--roof-only'],
        check_success=True,
        roof=True,
        app_name='app_1',
    )

    assert returncode == 0

    # Verify roofline.csv exists
    roofline_csv = workload_dir / "roofline.csv"
    assert roofline_csv.exists(), "roofline.csv should be generated"

    # Verify NO HTML files generated
    html_files = list(workload_dir.glob("*.html"))
    assert len(html_files) == 0, (
        f"Profile mode should NOT generate HTML. Found: {html_files}"
    )

    print("✓ Profile mode correctly skipped HTML generation")
```

#### Test 2: Analyze Creates Both ASCII and HTML
```python
def test_analyze_roofline_creates_ascii_and_html(
    binary_handler_analyze_rocprof_compute,
    tmp_path,
    sample_roofline_workload  # Fixture with roofline.csv
):
    """Verify analyze mode creates both ASCII and HTML charts"""

    # Run analyze mode
    returncode = binary_handler_analyze_rocprof_compute(
        arguments=['analyze', '-p', str(sample_roofline_workload)]
    )

    assert returncode == 0

    # Verify HTML file exists
    html_files = list(sample_roofline_workload.glob("empirRoof_*.html"))
    assert len(html_files) > 0, "Analyze should generate HTML charts"

    print(f"✓ Analyze mode generated HTML: {html_files}")
```

---

### Step 4: Update Documentation

**CRITICAL**: Multiple documentation files incorrectly describe roofline HTML generation in profile mode.

#### 4a. Update Profile Mode Documentation

**File**: `docs/how-to/profile/mode.rst`

**Changes required** (~15 locations):

1. **Line 225-228** - Remove HTML output reference:
```rst
# BEFORE:
Roofline-specific benchmark results are stored in ``roofline.csv`` and roofline
plots are outputted into HTMLs as ``empirRoof_gpu-<device ID><datatypes><kernels>.html``.

# AFTER:
Roofline-specific benchmark results are stored in ``roofline.csv``.
To generate roofline HTML visualization, use: ``rocprof-compute analyze -p <workload_dir>``
```

2. **Line 688-692** - Update --roof-only description:
```rst
# BEFORE:
This option checks if there is existing profiling data (``pmc_perf.csv`` and
``roofline.csv``): a) If found, uses the data files to create another roofline
HTML output; otherwise, b) Profile mode runs...

# AFTER:
This option checks if ``roofline.csv`` exists: a) If found, skips benchmarking;
otherwise, b) Profile mode runs roofline benchmarks only.
To visualize results: ``rocprof-compute analyze -p <workload_dir>``
```

3. **Line 776** - REMOVE deprecation warning (it's happening now!):
```rst
# BEFORE:
Deprecation warning: Standalone Roofline Analysis plot output will be
auto-generated in analyze mode instead of profile mode in a future release.

# AFTER:
Note: Roofline HTML generation (``empirRoof_*.html``) is performed in analyze mode.
Run ``rocprof-compute analyze`` after profiling to generate visualizations.
```

4. **Line 791** - Update workflow instructions:
```rst
# BEFORE:
...you can re-run the profiling command with each data type as long as the
``roofline.csv`` file still exists in the workload folder.

# AFTER:
After profiling with ``--roof-only``, use analyze mode to generate HTML
visualizations: ``rocprof-compute analyze -p <workload_dir>``. The
``roofline.csv`` file persists across multiple analyze runs.
```

5. **Lines 238, 317, 359, 785, 1215, 1264, 1311** - File tree examples:
```rst
# BEFORE (profile output):
├── empirRoof_gpu-0_FP32.html  # ❌ REMOVE
├── roofline.csv
├── pmc_perf.csv

# AFTER (profile output):
├── roofline.csv
├── pmc_perf.csv
# Note: Add analyze step example showing HTML generation
```

#### 4b. Update Analyze Mode Documentation

**File**: `docs/how-to/analyze/cli.rst`

**Add note** after line 64 (Empirical hierarchical roofline section):

```rst
.. note::

   **Roofline HTML Visualization**

   Starting from this release, roofline HTML files (``empirRoof_*.html``) are
   generated during analysis, not profiling.

   **Updated Workflow**:

   1. **Profile**: ``rocprof-compute profile --roof-only -- ./app``

      - Output: ``roofline.csv`` (benchmark data)

   2. **Analyze**: ``rocprof-compute analyze -p <workload_dir>``

      - Output: ``empirRoof_gpu-0_FP32.html`` (visualization)

   **Benefits**:

   - Faster profiling (no visualization overhead)
   - Regenerate visualizations without re-profiling
   - Profile on compute nodes, analyze on login nodes
```

#### 4c. Update Quickstart Guide

**File**: `docs/install/quickstart.rst`

**Line 119** - Update roofline description:
```rst
# BEFORE:
The application runs multiple times to collect all required performance counters.
Roofline analysis runs automatically unless you disable it using ``--no-roof``.

# AFTER:
The application runs multiple times to collect all required performance counters.
Roofline benchmarking runs automatically unless disabled with ``--no-roof``.
To generate roofline HTML visualizations, run ``rocprof-compute analyze -p <workload_dir>``
after profiling.
```

**Line 129** - Update profiling phase description:
```rst
# BEFORE:
During the profiling phase, roofline analysis also executes multiple iterations
to collect the necessary performance data.

# AFTER:
During the profiling phase, roofline benchmarking executes to collect performance
data into ``roofline.csv``. HTML visualization is generated during analyze mode.
```

**Line 147-150** - Add analyze step to example:
```rst
Collect only roofline data for performance analysis

.. code-block:: shell-session

    $ rocprof-compute profile --name vcopy --roof-only -- ./vcopy -n 1048576 -b 256
    $ rocprof-compute analyze -p workloads/vcopy  # Generate HTML visualization
```

**Line 250** - Clarify analysis command:
```rst
# BEFORE:
Show or display System speed-of-light (2) and roofline (4) analysis

# AFTER:
Show System speed-of-light (2) and roofline (4) analysis from profiled data

.. code-block:: shell-session

    $ rocprof-compute analyze -p workloads/vcopy -b 2 4
```

#### 4d. Documentation Summary

**Files to modify**: 3 core files
- `docs/how-to/profile/mode.rst` - ~15-20 changes (text + file tree examples)
- `docs/how-to/analyze/cli.rst` - 1 new note section
- `docs/install/quickstart.rst` - ~5 changes

**Key messaging throughout**:
- ✅ Profile mode: Creates `roofline.csv` (benchmark data only)
- ✅ Analyze mode: Creates `empirRoof_*.html` (visualizations)
- ✅ Workflow: Two-step process (profile → analyze)
- ✅ Benefit: Lighter profile, regenerate viz without re-profiling

1. **Profile** - Collect microbenchmark data:
   ```bash
   rocprof-compute profile --roof-only -- ./my_app
   ```

2. **Analyze** - Generate visualizations:
   ```bash
   rocprof-compute analyze -p ./workload_dir
   ```

This separation allows profile mode to run without heavy visualization dependencies.
```

---

## Impact Analysis

### Code Reduction
- **Total lines deleted**: ~390 lines
  - `soc_base.py`: 37 lines (post-profiling cleanup)
  - `roofline_calc.py`: 353 lines (`calc_ai_profile()` eliminated)
- **Total lines added/modified**: ~75 lines
  - `roofline.py`: 50 lines (refactored signatures)
  - `analysis_webui.py`: 10 lines (new API)
  - `analysis_cli.py`: 15 lines (new API + HTML generation)
- **Net reduction**: ~315 lines of code eliminated

### Dependencies Eliminated from Profile Mode
- ✅ plotly (HTML generation)
- ✅ dash, dash-bootstrap-components, dash-svg (Web UI components)
- ✅ plotext, plotille (ASCII plotting - moved to analyze)
- ✅ pandas operations for PMC loading (moved to analyze)

### Code Quality Improvements
- ✅ Eliminated duplicate AI calculation logic (`calc_ai_profile` vs `calc_ai_analyze`)
- ✅ Consistent naming (`html_generate_plot` + `cli_generate_plot`)
- ✅ Clean separation: AI calculation → visualization
- ✅ Single source of truth for roofline metrics (YAML-based)

### Dependencies Still in Profile Mode (addressed in later phases)
- ⚠️ pandas (for CSV joining in profiler_base.py - Phase 2)
- ⚠️ pyyaml (for config writing - Phase 3)

### User-Visible Changes

**Before** (current behavior):
```bash
$ rocprof-compute profile --roof-only -- ./app
...
[Deprecation Warning about future changes]
Generated roofline plot: empirRoof_gpu-0.html
```

**After** (new behavior):
```bash
$ rocprof-compute profile --roof-only -- ./app
...
✓ Roofline benchmark data saved to ./workload/roofline.csv
  Run 'rocprof-compute analyze -p ./workload' to generate roofline charts

$ rocprof-compute analyze -p ./workload
...
4.3 Roofline Plot:
[ASCII chart displayed]
✓ Roofline HTML chart saved to ./workload/empirRoof_gpu-0_FP32.html
```

### Backward Compatibility

**Breaking Change**: Users expecting HTML from profile mode must now run analyze

**Migration**:
```bash
# Old workflow:
rocprof-compute profile --roof-only -- ./app
# → HTML generated immediately

# New workflow:
rocprof-compute profile --roof-only -- ./app
rocprof-compute analyze -p ./workload
# → HTML generated in analyze step
```

**Mitigation**: Clear console messages guide users to analyze step

---

## Testing Plan

### Unit Tests
1. `test_profile_roofline_creates_csv_only` - Verify CSV only, no HTML
2. `test_analyze_roofline_creates_ascii_and_html` - Verify both charts

### Manual Testing

**Test 1: Profile mode without HTML**
```bash
cd /app/projects/rocprofiler-compute
python3 src/rocprof-compute profile --roof-only -n test1 -- /bin/true

# Verify:
ls -la ./test1/roofline.csv      # Should exist
ls -la ./test1/*.html             # Should be empty
```

**Test 2: Analyze mode with HTML**
```bash
python3 src/rocprof-compute analyze -p ./test1

# Verify:
ls -la ./test1/empirRoof_*.html   # Should exist
# Check console output for ASCII chart and HTML log message
```

**Test 3: Full workflow with real workload**
```bash
# Profile
rocprof-compute profile --roof-only -n vcopy_test -- ./tests/vcopy

# Analyze
rocprof-compute analyze -p ./vcopy_test

# Verify both ASCII and HTML charts
```

### Regression Testing
```bash
# Run existing roofline tests
pytest tests/ -k roofline -v

# Verify all tests pass with new workflow
```

---

## Success Criteria

- [ ] Profile mode creates only `roofline.csv` (no HTML)
- [ ] Profile mode does NOT import plotly, dash, plotext, or plotille
- [ ] Profile mode does NOT load PMC data or apply multiplexing
- [ ] Analyze mode generates both ASCII and HTML roofline charts
- [ ] HTML chart saved to workload directory with INFO log
- [ ] Test `test_profile_roofline_creates_csv_only` passes
- [ ] Test `test_analyze_roofline_creates_ascii_and_html` passes
- [ ] Documentation updated with two-step workflow
- [ ] All existing tests pass

---

## Files Modified Summary

### Code Changes
1. `src/rocprof_compute_soc/soc_base.py` - Simplify `post_profiling()` (**37 lines deleted**)
2. `src/utils/roofline_calc.py` - Delete `calc_ai_profile()` (**353 lines deleted**)
3. `src/roofline.py` - Rename `empirical_roofline()` → `html_generate_plot()` and refactor signatures (~50 lines modified)
4. `src/rocprof_compute_analyze/analysis_webui.py` - Update caller to use new API (~10 lines modified)
5. `src/rocprof_compute_analyze/analysis_cli.py` - Update caller to use new API, add HTML generation (~15 lines modified)

### Test Changes
3. `tests/test_roofline.py` - New tests for profile/analyze separation

### Documentation Changes
4. `docs/profiling.rst` - Document two-step workflow
5. `README.md` - Update workflow section
6. `CHANGELOG.md` - Document breaking change

---

## Rollback Plan

If issues arise:
1. Revert `soc_base.py` changes (restore lines 720-757)
2. Revert `roofline.py` changes (remove HTML generation block)
3. Previous behavior fully restored

---

## PR Description Template

```markdown
## Phase 1: Move ALL Roofline Post-Processing to Analyze Mode

### Summary
Complete separation and refactoring of roofline workflow:
1. Profile mode now only runs microbenchmarks → creates `roofline.csv`
2. All AI calculation and visualization moved to analyze mode
3. Eliminated duplicate roofline logic (`calc_ai_profile` removed, ~353 lines)
4. Standardized roofline API (`empirical_roofline` → `html_generate_plot`)
5. Clean separation: calculate AI once, use for both HTML and ASCII charts

**Net impact**: ~315 lines of code deleted, cleaner architecture, zero profile dependencies

### Changes

**Profile Mode** (`soc_base.py`):
- Removed PMC data loading (was using pandas)
- Removed multiplexing operations
- Removed roofline HTML generation
- Removed deprecation warning
- **37 lines deleted**

**Roofline Refactoring** (`roofline.py` + `roofline_calc.py`):
- **Deleted `calc_ai_profile()`** - 353 lines eliminated
- **Renamed `empirical_roofline()` → `html_generate_plot()`** - consistent naming
- **Moved AI calculation outside plot functions** - calculate once, use twice
- **Standardized on workload-based approach** - both modes use `calc_ai_analyze()`
- Refactored function signatures for clarity

**Analyze Mode Updates** (`analysis_cli.py`, `analysis_webui.py`):
- Both modes call `calc_ai_analyze()` first
- WebUI calls `html_generate_plot()` for interactive charts
- CLI calls both `html_generate_plot()` (standalone mode) and `cli_generate_plot()` (ASCII)
- Single source of truth for AI calculation

### Impact

**Removed from profile path**:
- plotly (HTML generation)
- dash family (Web UI components)
- plotext, plotille (ASCII plotting)
- pandas operations (PMC loading)

**User-visible change**:
- Profile creates CSV only
- Analyze creates both ASCII and HTML charts
- Users must run analyze after profile to get visualizations

### Migration

Old: `rocprof-compute profile --roof-only -- ./app` → HTML generated
New: `rocprof-compute profile --roof-only -- ./app && rocprof-compute analyze -p ./workload` → HTML in analyze

### Testing
- [x] Profile creates CSV only, no HTML
- [x] Analyze creates both ASCII and HTML
- [x] HTML matches ASCII data
- [x] All existing tests pass
- [x] Documentation updated

### Related
- Part of BU requirement: Profile mode zero dependencies
- Dependency: None (independent PR)
- Follow-up: Phase 2 (pandas), Phase 3 (yaml)
```

---

## Notes

- **Comprehensive refactoring**: Not just moving code, but eliminating duplication
- **Major code reduction**: ~315 net lines deleted (390 deleted, 75 added)
- **Cleaner architecture**: Single AI calculation method, consistent naming, clear separation
- **More dependencies removed**: plotly, dash, plotext, plotille, pandas PMC ops
- **Lower risk**: Analyze already has the logic, we're just organizing it better
- **Better maintainability**: No more `calc_ai_profile` vs `calc_ai_analyze` confusion
- **Better UX**: Clear two-step workflow with helpful messages
- **Independent PR**: No conflicts with other phases
- **Complete package**: Code + tests + docs all updated together
