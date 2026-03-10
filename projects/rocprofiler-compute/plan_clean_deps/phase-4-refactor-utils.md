# Phase 4: Refactor utils.py into Mode-Specific Modules

**PR #4** | **Theme**: Organize stdlib-only utilities
**Objective**: Split utils.py into clear profile vs analyze modules
**Dependencies**: PR #2, #3 merged (pandas and yaml eliminated)
**Duration**: 2-3 days
**Status**: Skeleton - Details TBD

---

## Problem Statement

`src/utils/utils.py` is 2251 lines with mixed profile and analyze functions:
- Profile functions (stdlib-only after Phases 2-3)
- Analyze functions (pandas, numpy, heavy dependencies)
- Shared utilities (used by both modes)

This creates confusion about what's safe to use in profile mode and makes it harder to maintain the stdlib-only guarantee.

---

## Objective

Split `utils/utils.py` into three focused modules:

1. **`utils/utils_common.py`** - Shared stdlib utilities
   - `format_time()`, `get_uuid()`, `print_status()`
   - `capture_subprocess_output()`
   - File I/O helpers
   - Logging utilities

2. **`utils/utils_profile.py`** - Profile-specific (stdlib only)
   - `run_prof()` - Core profiling execution
   - `pc_sampling_prof()` - PC sampling
   - `gen_sysinfo()` - System info generation
   - CSV utilities (from Phase 2)

3. **`utils/utils_analysis.py`** - Analysis-specific (heavy deps OK)
   - `impute_counters_iteration_multiplex()` - Pandas operations
   - `merge_counters_spatial_multiplex()` - Pandas operations
   - `process_torch_trace_output()` - Pandas operations
   - Metric evaluation functions

---

## Scope

### In Scope
- Create three new utility modules
- Move functions to appropriate modules
- Update all import statements across codebase
- Tests to verify no import errors
- Documentation of new module structure

### Out of Scope
- Functional changes to utilities (keep behavior identical)
- New utilities or features
- Performance optimization

---

## Success Criteria

- [ ] Three new modules created: `utils_common.py`, `utils_profile.py`, `utils_analysis.py`
- [ ] `utils_profile.py` contains ONLY stdlib-only functions
- [ ] `utils_analysis.py` clearly separated with heavy dependencies
- [ ] All ~100 import statements updated across codebase
- [ ] Compatibility shim (`utils/utils.py`) works during migration
- [ ] **All existing tests pass** with new module structure ✅
- [ ] No circular import dependencies
- [ ] No new tests added (Phase 4 is refactoring only)

**Note**: Stdlib-only validation tests are added in Phase 5, not Phase 4.

---

## Files to Create (Preliminary)

- `src/utils/utils_common.py` - Shared utilities
- `src/utils/utils_profile.py` - Profile-specific utilities
- `src/utils/utils_analysis.py` - Analysis-specific utilities

## Files to Modify (Preliminary)

- All files importing from `utils.utils` (update import paths)
- `src/rocprof_compute_profile/profiler_base.py`
- `src/analysis_base.py`
- `src/analysis_cli.py`
- `src/roofline.py`
- Many others (comprehensive import update)

---

## Migration Strategy

1. Create new module files with functions categorized
2. Keep `utils/utils.py` temporarily as compatibility shim:
   ```python
   # utils/utils.py (deprecated)
   from utils.utils_common import *
   from utils.utils_profile import *
   from utils.utils_analysis import *
   # TODO: Remove this file after all imports updated
   ```
3. Update imports incrementally
4. Remove shim file once all imports migrated
5. Update tests

---

## Implementation Plan

### Step 1: Analyze Current utils.py Usage

**Scan all imports** across codebase:
```bash
grep -r "from utils import\|from utils.utils import" src/ tests/ --include="*.py" | wc -l
# Expected: 100+ files
```

Categorize all ~150 functions in `utils/utils.py` into:
- Common (used by both modes, stdlib only)
- Profile-specific (stdlib only)
- Analysis-specific (can use pandas/numpy)

---

### Step 2: Create Three New Modules

**File**: `src/utils/utils_common.py`

Move shared stdlib utilities (~40 functions):
- `format_time()`, `get_uuid()`, `print_status()`
- `capture_subprocess_output()`
- `get_version()`, `get_build_info()`
- File I/O helpers: `load_yaml()`, path utilities
- Logging utilities

**File**: `src/utils/utils_profile.py`

Move profile-specific utilities (~30 functions):
- `run_prof()` - Core profiling execution
- `pc_sampling_prof()` - PC sampling
- `gen_sysinfo()` - System info generation
- `add_counter_extra_config_input_yaml()` - Counter definition builder
- Perfmon helpers

**File**: `src/utils/utils_analysis.py`

Move analysis-specific utilities (~80 functions):
- `impute_counters_iteration_multiplex()` - Pandas operations
- `merge_counters_spatial_multiplex()` - Pandas operations
- `create_df_pmc()` - DataFrame creation
- `process_torch_trace_output()` - Pandas operations
- Metric evaluation functions
- All pandas/numpy dependent code

---

### Step 3: Create Compatibility Shim

**File**: `src/utils/utils.py` (temporary)

Keep as compatibility shim during migration:
```python
"""
DEPRECATED: This file is a compatibility shim during utils refactoring.
Use specific imports instead:
  - from utils.utils_common import ...  # Shared utilities
  - from utils.utils_profile import ... # Profile-specific
  - from utils.utils_analysis import ... # Analysis-specific

This file will be removed after all imports updated.
"""

# Re-export everything for backward compatibility
from utils.utils_common import *
from utils.utils_profile import *
from utils.utils_analysis import *

import warnings
warnings.warn(
    "Importing from utils.utils is deprecated. "
    "Use utils_common, utils_profile, or utils_analysis instead.",
    DeprecationWarning,
    stacklevel=2
)
```

---

### Step 4: Update Imports Incrementally

**Priority order**:
1. Profile mode files first (critical path)
2. Analyze mode files
3. Test files last

**Pattern**:
```python
# BEFORE:
from utils.utils import run_prof, gen_sysinfo, create_df_pmc

# AFTER (profile files):
from utils.utils_profile import run_prof, gen_sysinfo
# Don't import create_df_pmc in profile mode!

# AFTER (analyze files):
from utils.utils_analysis import create_df_pmc
```

**Automated search/replace** (careful with this):
```bash
# Find all files that import from utils
find src/ tests/ -name "*.py" -exec grep -l "from utils" {} \;
```

---

### Step 5: Update Tests

**File**: `tests/test_utils.py` (264KB file!)

This massive test file likely tests ALL utils functions. Need to:

1. **Split test file** to mirror new structure:
   - `tests/test_utils_common.py`
   - `tests/test_utils_profile.py`
   - `tests/test_utils_analysis.py`

2. **Update imports** in each test file:
```python
# test_utils_profile.py
from utils.utils_profile import run_prof, gen_sysinfo
import pytest

def test_run_prof():
    # Test profile utilities
    pass

# test_utils_analysis.py
from utils.utils_analysis import create_df_pmc, impute_counters_iteration_multiplex
import pandas as pd
import pytest

def test_create_df_pmc():
    # Test analysis utilities
    pass
```

3. **Update test_utils.py helper file**:
```python
# tests/test_utils.py (test helper, not tests)
# Update imports used by test helpers
from utils.utils_common import format_time, get_uuid
# etc.
```

**Other test files** (~12 files):
- `test_profile_general.py` - Update to use `utils_profile`
- `test_analyze_commands.py` - Update to use `utils_analysis`
- `test_data_imputation.py` - Update to use `utils_analysis`
- etc.

---

### Step 6: Remove Compatibility Shim

Once all imports updated:
1. Delete `src/utils/utils.py`
2. Run full test suite
3. Verify no import errors

---

### Step 7: Verify All Tests Pass

**Objective**: Ensure all existing tests pass with new import structure (no new tests added)

**Run full test suite**:
```bash
cd /app/projects/rocprofiler-compute
pytest tests/ -v
```

**Expected result**: ALL tests pass ✅

**If tests fail**:
- Check import statements in failing test files
- Verify compatibility shim (`utils/utils.py`) is still active
- Fix imports incrementally

**Note**: Phase 4 does NOT add new guard tests for stdlib-only validation. Those belong in Phase 5 (final test infrastructure). Phase 4 only ensures existing functionality works with the refactored structure.

---

## Implementation Details

### Function Categorization (Examples)

**Common** (stdlib only, used by both):
- `format_time()` - time formatting
- `get_uuid()` - UUID generation
- `capture_subprocess_output()` - subprocess handling
- `console_log()`, `console_error()` - logging
- `get_version()` - version info

**Profile** (stdlib only, profile-specific):
- `run_prof()` - rocprof execution
- `pc_sampling_prof()` - PC sampling
- `gen_sysinfo()` - system info
- `add_counter_extra_config_input_yaml()` - counter defs
- Perfmon file generation helpers

**Analysis** (heavy deps OK, analysis-specific):
- `create_df_pmc()` - DataFrame creation (pandas)
- `impute_counters_iteration_multiplex()` - data imputation (pandas)
- `merge_counters_spatial_multiplex()` - merging (pandas)
- `process_torch_trace_output()` - trace processing (pandas)
- All metric evaluation functions

### Test File Impact

**~100 files** will need import updates:
- Profile mode: ~20 files
- Analyze mode: ~30 files
- SoC modules: ~10 files
- Tests: ~12 files
- Utils/helpers: ~10 files
- Others: ~18 files

---

## Notes

- Cleanup/organizational phase after major refactoring
- Low risk (behavior unchanged, just reorganization)
- Makes stdlib-only guarantee clear and maintainable
- Easier code review for future changes
