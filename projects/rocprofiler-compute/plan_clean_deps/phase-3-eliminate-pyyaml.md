# Phase 3: Eliminate PyYAML from Profile Mode

**PR #3** | **Theme**: Create minimal YAML library for profile mode
**Objective**: Remove external pyyaml dependency from profile code path
**Dependencies**: PR #2 merged (easier without pandas conflicts)
**Duration**: 3-5 days
**Status**: Ready to implement

---

## Problem Statement

After Phase 1 & 2, profile mode still has **3 YAML usage points** requiring external pyyaml:

### 1. Writing profiling configuration (profiler_base.py line 477)
```python
with open(f"{self.__args.path}/profiling_config.yaml", "w") as f:
    args_dict = vars(self.__args)
    args_dict["filter_blocks"] = self._filter_blocks
    args_dict["config_dir"] = str(args_dict["config_dir"])
    yaml.dump(args_dict, f)
```
**Data**: Simple dict with strings, ints, bools, Path objects

### 2. Reading/dumping analysis configs (soc_base.py detect_counters(), lines 302-336)
Called during `profiling_setup()` → `perfmon_filter()` → `detect_counters()`:
```python
file_config = yaml.safe_load(stream)  # Load SoC config YAML
yaml.dump(file_config, sort_keys=False)  # Dump filtered sections
yaml.dump(panel_dict[panel_id], sort_keys=False)  # Dump panel
yaml.dump(metric_dict[metric_id], sort_keys=False)  # Dump metric
```
**Data**: Nested dicts with strings, lists, complex expressions

### 3. Writing perfmon counter definitions (soc_base.py line 656)
Called during `perfmon_filter()` → `perfmon_coalesce()`:
```python
counter_def = {
    "rocprofiler-sdk": {
        "counters-schema-version": 1,
        "counters": [{
            "name": "TCC_HIT[0]",
            "description": "TCC_HIT on 0th XCC and 0th channel",
            "properties": [],
            "definitions": [{
                "architectures": ["gfx942"],
                "expression": "select(TCC_HIT,[DIMENSION_XCC=[0], DIMENSION_INSTANCE=[0]])"
            }]
        }]
    }
}
yaml.dump(counter_def, sort_keys=False)
```
**Data**: Nested dict/list structure for TCC channel counters

### 4. Reading/merging counter definitions (utils.py run_prof(), lines 918-933)
```python
counter_defs = yaml.safe_load(file)  # Load base counter_defs.yaml
counter_defs["rocprofiler-sdk"]["counters"].extend(...)  # Merge
yaml.dump(counter_defs, tmpfile, default_flow_style=False, sort_keys=False)
```
**Data**: Same structure as #3

**Current Dependency**: `pyyaml==6.0.3` (~50KB package)

---

## Objective

**Create minimal YAML library** (`src/utils/yaml_lib.py`) with only `safe_load()` and `dump()` for basic types.

**Why this approach:**
- ✅ We only use 2 functions: `yaml.safe_load()` and `yaml.dump()`
- ✅ All YAML data uses only: dicts, lists, strings, ints, bools, None (no anchors, tags, etc.)
- ✅ ~300 line single-file implementation sufficient
- ✅ Based on PyYAML patterns but not full vendoring (no LICENSE needed, just attribution)
- ✅ Supports Python 3.8-3.12
- ✅ Zero external dependencies

**Rejected alternatives:**
- ❌ Vendor full PyYAML: Overkill (~20 files, 5000+ lines for 2 functions)
- ❌ Convert to JSON: YAML format is part of rocprofiler interface, can't change

---

## Scope

### In Scope
- Create `src/utils/yaml_lib.py` with `safe_load()` and `dump()`
- Support parameters: `sort_keys=False`, `default_flow_style=None`
- Update 3 import statements: profiler_base.py, soc_base.py, utils.py
- Test all 4 YAML usage points still work
- Validate against Python 3.8-3.12

### Out of Scope
- YAML usage in analyze mode (can still use external pyyaml)
- Advanced YAML features (anchors, tags, custom types)
- Optimizing YAML parsing performance

---

## Implementation Plan

### Step 1: Create Minimal YAML Library

**File**: `src/utils/yaml_lib.py` (new file, ~300-400 lines)

Create single-file minimal YAML library based on PyYAML's safe subset:

```python
"""
Minimal YAML Library for rocprof-compute
Based on PyYAML 6.0.3 safe_load() and dump() implementations
Source: https://github.com/yaml/pyyaml/tree/6.0.3
Commit: c1a91bc2e756c091e4e05b84b8ec31e88e5630f (example)

This is a minimal pure-Python YAML implementation supporting only:
- Dicts (mappings)
- Lists (sequences)
- Strings, integers, floats, booleans, None
- Basic YAML syntax (no anchors, tags, or advanced features)

Sufficient for rocprof-compute's profiling configs and counter definitions.
Compatible with Python 3.8+.
"""

def safe_load(stream):
    """
    Load YAML from string or file stream.
    Supports only safe basic types (dict, list, str, int, float, bool, None).
    """
    # Implementation: Simple recursive descent parser
    # Parse YAML syntax into Python objects
    pass

def dump(data, stream=None, sort_keys=False, default_flow_style=None):
    """
    Dump Python data to YAML format.

    Args:
        data: Python dict/list/str/int/float/bool/None
        stream: File object to write to (or None for string return)
        sort_keys: Whether to sort dict keys (default False)
        default_flow_style: Use flow style (inline) if True (default None/block)

    Returns:
        YAML string if stream is None, otherwise None
    """
    # Implementation: Recursive serializer
    # Convert Python objects to YAML syntax
    pass
```

**Testing**: Create `tests/test_yaml_lib.py` to validate against real YAML files used in profiling.

---

### Step 2: Update All YAML Imports

**Files to modify**:
1. `src/rocprof_compute_profile/profiler_base.py`
2. `src/rocprof_compute_soc/soc_base.py`
3. `src/utils/utils.py`

**Change**:
```python
# BEFORE:
import yaml

# AFTER:
from utils import yaml_lib as yaml
```

**Impact**: ~3 files, 3 lines changed

---

### Step 3: Verify YAML Operations Still Work

**Test all YAML usage points**:

1. **profiler_base.py line 477** - Write profiling config
2. **soc_base.py lines 302-336** - Read/dump analysis configs
3. **soc_base.py line 656** - Write counter definitions
4. **utils.py lines 918-933** - Read/merge/write counter definitions

Run existing profile tests to ensure no regressions.

---

### Step 4: Update Tests

**File**: `tests/test_yaml_lib.py` (new)

```python
import pytest
from pathlib import Path
from utils import yaml_lib as yaml

def test_yaml_lib_dump_basic_dict():
    """Test dumping basic dictionary"""
    data = {'key1': 'value1', 'key2': 123, 'key3': True}
    result = yaml.dump(data)
    assert 'key1: value1' in result
    assert 'key2: 123' in result
    assert 'key3: true' in result

def test_yaml_lib_dump_nested():
    """Test dumping nested structures"""
    data = {
        'rocprofiler-sdk': {
            'counters-schema-version': 1,
            'counters': [
                {
                    'name': 'TCC_HIT[0]',
                    'description': 'Test counter',
                    'properties': [],
                    'definitions': [
                        {'architectures': ['gfx942'], 'expression': 'test'}
                    ]
                }
            ]
        }
    }
    result = yaml.dump(data, sort_keys=False)
    loaded = yaml.safe_load(result)
    assert loaded == data

def test_yaml_lib_safe_load_config():
    """Test loading actual profiling config YAML"""
    yaml_content = """
    kernel_name: vecCopy
    device: 0
    iteration_multiplexing: null
    spatial_multiplexing: false
    """
    result = yaml.safe_load(yaml_content)
    assert result['kernel_name'] == 'vecCopy'
    assert result['device'] == 0
    assert result['iteration_multiplexing'] is None
    assert result['spatial_multiplexing'] is False

def test_yaml_lib_roundtrip_counter_defs():
    """Test roundtrip of counter definition structure"""
    # This mirrors actual usage in soc_base.py line 656
    counter_def = {
        'rocprofiler-sdk': {
            'counters-schema-version': 1,
            'counters': [
                {
                    'name': 'TEST_COUNTER',
                    'description': 'Test description',
                    'properties': [],
                    'definitions': [{
                        'architectures': ['gfx942'],
                        'expression': 'select(TCC_HIT,[DIMENSION_XCC=[0]])'
                    }]
                }
            ]
        }
    }

    # Dump to YAML
    yaml_str = yaml.dump(counter_def, sort_keys=False)

    # Load back
    loaded = yaml.safe_load(yaml_str)

    # Verify identical
    assert loaded == counter_def

def test_profile_no_external_yaml():
    """
    CRITICAL: Verify profile mode doesn't import external pyyaml
    """
    import sys

    # Clear pyyaml if imported
    if 'yaml' in sys.modules and 'pyyaml' in str(sys.modules['yaml']):
        del sys.modules['yaml']

    # Import profile mode
    from rocprof_compute_profile.profiler_base import RocProfCompute_Base

    # yaml should be yaml_lib, not external pyyaml
    import yaml
    assert 'yaml_lib' in str(yaml.__file__), (
        f"Profile imported external pyyaml! Got: {yaml.__file__}"
    )
```

**Update existing tests**: Verify all profile tests still pass with `yaml_lib`.

---

## Success Criteria

- [ ] `utils/yaml_lib.py` created with safe_load() and dump()
- [ ] Profile mode imports `yaml_lib` instead of external `pyyaml`
- [ ] `profiling_config.yaml` written correctly
- [ ] Counter definitions loaded/written successfully
- [ ] All 3 YAML usage points verified working
- [ ] All profile tests pass
- [ ] test_yaml_lib.py validates roundtrip correctness
- [ ] **After this PR: Profile has ZERO external dependencies** 🎉

---

## Files to Modify (Preliminary)

**Option A (Vendoring)**:
- `src/utils/vendored/yaml/` (new) - PyYAML source
- `src/rocprof_compute_profile/profiler_base.py` - Import from vendored
- `LICENSE.md` - Add PyYAML attribution

**Option B (JSON)**:
- `src/rocprof_compute_profile/profiler_base.py` - Use json module
- `CMakeLists.txt` - Convert YAML to JSON at build time
- Counter definition files - Provide JSON alternatives

---

## Decision Criteria

**Favor Option A (Vendoring) if**:
- YAML human-readability important
- Existing YAML configs extensive
- PyYAML license compatible (MIT ✓)

**Favor Option B (JSON) if**:
- Simplicity preferred
- Config files rarely hand-edited
- Want truly zero external code

---

## Implementation Details

**To be filled in during execution based on**:
- Phase 2 completion (pandas removed, cleaner codebase)
- Analysis of all YAML usage points
- BU preference for YAML vs JSON
- License review for vendoring

---

## Notes

- **Milestone**: After this phase, profile mode achieves ZERO external dependencies
- Depends on Phase 2 merged to avoid merge conflicts
- Relatively low-risk change (YAML usage well-defined)
