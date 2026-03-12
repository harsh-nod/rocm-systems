# Phase 1: Move Roofline Visualization to Analyze Mode

**JIRA**: AIPROFCOMP-107, AIPROFCOMP-29
**Dependencies**: None
**Status**: Ready to implement

## Summary

Move roofline HTML generation and AI calculation from profile mode to analyze mode. Profile mode will only run microbenchmarks and create `roofline.csv`. This eliminates plotly, dash, plotext dependencies from profile path.

**Dependencies Removed from Profile**: plotly, dash, dash-bootstrap-components, dash-svg, plotext, plotille

## Changes Overview

**Profile Mode**:
- `soc_base.py`: Remove PMC loading, multiplexing, deprecation warning; keep only microbenchmark execution
- `rocprof_compute_base.py`: Remove duplicate post_profiling() call
- `argparser.py`: Keep only --roof-only, --device in profile mode

**Analyze Mode**:
- `roofline.py`: Rename functions, clean constructor, delete dead code, add validation
- `roofline_calc.py`: Delete duplicate `calc_ai_profile()` (353 lines)
- `soc_base.py`: Remove roofline_obj from constructor; created only via analysis_setup()
- `analysis_cli.py`, `analysis_webui.py`: Call calc_ai_analyze(), pass ai_data to plot functions
- `argparser.py`: Move --mem-level, --roofline-data-type, --sort to analyze mode

**Tests**:
- Update 11+ existing tests (remove HTML expectations from profile)
- Add edge case tests (missing CSV, corrupted CSV, idempotency)

**Documentation**:
- Update 3 docs files with two-step workflow
- Add CHANGELOG entry

---

## Implementation Plan

### Step 1: Remove duplicate `post_profiling()` call

**File**: `src/rocprof_compute_base.py`

**Delete line 556**: `self.__soc[self.__mspec.gpu_arch].post_profiling()`

**Reason**: Duplicate call - `profiler.post_processing()` at line 551 already calls `soc.post_profiling()` internally (see profiler_base.py:822). This causes roofline microbenchmarks to run twice.

---

### Step 2: Simplify `soc_base.py::post_profiling()`

**File**: `src/rocprof_compute_soc/soc_base.py`

**Changes to `post_profiling()` method**:

**Delete lines 687-694** (pmc_perf.csv check - no longer needed):
```python
pmc_path = Path(self.get_args().path) / "pmc_perf.csv"
if not pmc_path.is_file():
    console_error(
        "roofline",
        "Incomplete or missing profiling data. Skipping roofline.",
        exit=False,
    )
    return
```

**Delete lines 720-756** (deprecation warning + PMC loading + post_processing call):
- Lines 720-729: Deprecation warning
- Lines 731-756: PMC data loading, multiplexing, post_processing() call

**Add after roofline.csv validation** (after the `return` statement at line 718):
```python
console_log(
    "roofline",
    f"✓ Roofline data saved to {self.get_args().path}/roofline.csv\n"
    f"  Run 'rocprof-compute analyze -p {self.get_args().path}' for charts"
)
```

**Result**: Keep only skip checks (lines 673-682), microbenchmark execution (lines 698-709), and validation (lines 710-718)

---

### Step 3: Split Roofline Argparse Options

**File**: `src/argparser.py`

**Create separate roofline argument groups**:
```python
# Profile mode roofline options
roofline_group_profile = profile_parser.add_argument_group("Roofline Options")

# Analyze mode roofline options
roofline_group_analysis = analyze_parser.add_argument_group("Roofline Options")
```

**Profile mode roofline options** (microbenchmark related):
- `--roof-only` - Run only roofline microbenchmarks
- `--device` - Target GPU device ID for microbenchmarks
- `--no-roof` - Skip roofline.csv creation entirely (stays in profile_group)

**Analyze mode roofline options** (visualization related):
- `-m`, `--mem-level` - Filter by memory level (HBM, L2, vL1D, LDS)
- `-R`, `--roofline-data-type` - Choose datatypes for HTML visualization (FP32, FP16, etc.)
- `--sort` - Overlay top kernels or dispatches

**Delete from argparser.py**:
- Lines 636-655: Commented-out roofline options (--workgroups, --wsize, --dataset, --experiments, --iter)

**Move from profile to analyze**:
- `--mem-level` (line 575-590)
- `--roofline-data-type` (line 600-634)
- `--sort` (line 562-574)

**Keep in profile only**:
- `--roof-only` (line 552-561)
- `--device` (line 592-599)
- `--no-roof` (line 468-473, in profile_group)

---

### Step 4: Refactor Roofline Functions and Constructor

**File**: `src/utils/roofline_calc.py`
- **Delete** `calc_ai_profile()` function entirely (353 lines) - duplicate logic

**File**: `src/roofline.py`
- **Rename** `empirical_roofline()` → `html_generate_plot()`
- **Update signatures** to accept pre-calculated `ai_data` dict parameter
- **Add validation** in both `html_generate_plot()` and `cli_generate_plot()`:
  - Check if roofline.csv exists
  - Return None gracefully if missing/invalid (don't crash analyze)
- **Delete dead code** (no longer called after Phase 1):
  - `post_processing()` method (lines 1343-1348)
  - `standalone_roofline()` method (lines 1326-1337)
  - Redundant comment above `post_processing()` (lines 1339-1341)
- **Clean up constructor**:
  - Make `run_parameters` required (remove Optional, remove default dict)
  - Delete arg parsing logic (lines 106-119) - args like `--no-roof`, `--mem-level`, `--sort` no longer parsed from args
  - Callers must provide complete `run_parameters` dict

**Reason**: After Phase 1, roofline is ONLY used in analyze mode. Analyze callers explicitly provide run_parameters via `analysis_setup()`.

**File**: `src/rocprof_compute_soc/soc_base.py`
- **Delete** roofline_obj creation from SOC constructor (lines 78-79):
  ```python
  # DELETE these lines (line 77 is just a comment, keep it):
  if hasattr(self.__args, "mode") and self.__args.mode:
      self.roofline_obj = Roofline(args, self._mspec)
  ```
- **Reason**: Roofline object should only be created by `analysis_setup()`, not in SOC constructor

**AI Calculation Flow**:
- **Before**: Each plot function calls AI calc internally
- **After**: Callers call `calc_ai_analyze()` once, pass result to both plot functions

---

### Step 5: Update Callers

**File**: `src/rocprof_compute_analyze/analysis_webui.py`
- Import `calc_ai_analyze` from utils.roofline_calc
- Already calls `analysis_setup()` with explicit `roofline_parameters` ✅
- Sets `is_standalone=False` in roofline_parameters (returns html.Section for WebUI)
- Call `calc_ai_analyze()` before calling plot functions
- Pass `ai_data` to `html_generate_plot()` (renamed from `empirical_roofline`)

**File**: `src/rocprof_compute_analyze/analysis_cli.py`
- Import `calc_ai_analyze` from utils.roofline_calc
- **Add `analysis_setup()` call** before using roofline_obj (currently missing!):
  ```python
  soc[gpu_arch].analysis_setup(
      roofline_parameters={
          "workload_dir": workload.path,
          "device_id": 0,
          "sort_type": args.sort,  # From analyze mode args
          "mem_level": args.mem_level,  # From analyze mode args
          "is_standalone": True,  # CLI mode - save HTML files to disk
          "roofline_data_type": args.roofline_data_type,
          "kernel_filter": False,
          "iteration_multiplexing": self._profiling_config["iteration_multiplexing"],
      }
  )
  ```
- Call `calc_ai_analyze()` once
- Pass `ai_data` to `html_generate_plot()` (for HTML file on disk) and `cli_generate_plot()` (for ASCII terminal display)
- Handle None returns gracefully (if roofline.csv missing)

---

### Step 6: Update Tests

**File**: `tests/test_profile_general.py`
- Update `ROOF_ONLY_FILES` constant - remove HTML expectation
- Update 11+ roofline tests - remove HTML file assertions, verify only CSV created
- Verify NO HTML files generated by profile mode

**File**: `tests/test_analyze_commands.py` - Add new tests:
- `test_analyze_generates_roofline_html` - Full workflow (profile → analyze)
- `test_analyze_roofline_multiple_datatypes` - Multiple data types
- `test_analyze_missing_roofline_csv_graceful` - Missing CSV edge case
- `test_analyze_roofline_idempotent` - Multiple analyze runs
- `test_analyze_corrupted_roofline_csv_graceful` - Corrupted CSV edge case

---

### Step 7: Update Documentation

**File**: `docs/how-to/profile/mode.rst`
- Remove HTML output references from roofline sections
- Update --roof-only description (creates CSV only)
- Remove deprecation warning text
- Update file tree examples (remove empirRoof_*.html from profile output)
- Add note: "Run analyze mode to generate HTML"

**File**: `docs/how-to/analyze/cli.rst`
- Add section documenting roofline HTML generation in analyze mode
- Show two-step workflow example (profile → analyze)

**File**: `docs/install/quickstart.rst`
- Update roofline workflow description
- Add analyze step to examples

**File**: `CHANGELOG.md`
- Add entry: "BREAKING: Roofline HTML visualization moved to analyze mode. Profile mode now creates only roofline.csv. Roofline options split: --mem-level, --roofline-data-type, --sort moved to analyze mode."

---

## Files Modified

**Code** (7 files):
1. `src/rocprof_compute_base.py` - Delete duplicate post_profiling() call (line 556)
2. `src/rocprof_compute_soc/soc_base.py` - Delete pmc check, PMC loading, deprecation, roofline_obj constructor creation
3. `src/argparser.py` - Split roofline options, delete commented-out options
4. `src/utils/roofline_calc.py` - Delete `calc_ai_profile()` (353 lines)
5. `src/roofline.py` - Rename function, clean constructor, delete dead code, add validation
6. `src/rocprof_compute_analyze/analysis_webui.py` - Update caller with calc_ai_analyze
7. `src/rocprof_compute_analyze/analysis_cli.py` - Add analysis_setup() call, update with calc_ai_analyze

**Tests** (2 files):
8. `tests/test_profile_general.py` - Update 11+ tests
9. `tests/test_analyze_commands.py` - Add 5 new tests

**Documentation** (4 files):
10. `docs/how-to/profile/mode.rst`
11. `docs/how-to/analyze/cli.rst`
12. `docs/install/quickstart.rst`
13. `CHANGELOG.md`

**Total**: 13 files modified, ~436 lines deleted, ~95 lines added

---

## Success Criteria

- [ ] Profile creates only roofline.csv (no HTML)
- [ ] Profile does NOT import plotly, dash, plotext, plotille
- [ ] Analyze generates both ASCII and HTML roofline charts
- [ ] Plot functions handle missing roofline.csv gracefully
- [ ] `calc_ai_profile()` deleted
- [ ] `empirical_roofline()` renamed to `html_generate_plot()`
- [ ] All profile tests pass (updated expectations)
- [ ] All new analyze tests pass
- [ ] Documentation updated
- [ ] CHANGELOG.md entry added

---
