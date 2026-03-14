# Phase 3: Vendor PyYAML

**Objective**: Vendor PyYAML 6.0.3 as git submodule to eliminate external dependency from profile-specific code
**Dependencies**: Phase 1 & 2 merged
**Duration**: 3-5 days

---

## Summary

**Scope**: Update 3 profile-specific files to use vendored PyYAML. Build vendoring infrastructure for future packages.

**Phase Boundaries**:
- `utils/utils.py` intentionally NOT updated (Phase 4 will segregate shared utilities)
- `requirements.txt` keeps pyyaml (analyze mode needs it)
- Full profile isolation achieved after Phase 4 completes

---

## Problem

Profile mode has 7 files importing yaml:

**Phase 3 scope (3 files):**
1. `src/rocprof_compute_profile/profiler_base.py` (line 38)
2. `src/rocprof_compute_soc/soc_base.py` (line 35)
3. `src/utils/mi_gpu_spec.py` (line 29)

**Phase 4 scope:**
4. `src/utils/utils.py` (line 53) - Shared file, segregate later

**Not modified:**
5. `src/utils/file_io.py` - Analyze only
6. `src/utils/hash_checker.py` - Dev tool
7. `src/rocprof_compute_tui/widgets/collapsibles.py` - TUI only

---

## Implementation

### 1. Add Git Submodule

```bash
git submodule add https://github.com/yaml/pyyaml.git src/vendored/pyyaml
cd src/vendored/pyyaml && git checkout 6.0.3 && cd ../../..
git add .gitmodules src/vendored/pyyaml
git commit -m "Vendor PyYAML 6.0.3"
```

### 2. Create Vendored Package Wrapper

**File**: `src/vendored/__init__.py`

```python
"""Vendored dependencies for rocprofiler-compute."""

# PyYAML 6.0.3 - https://github.com/yaml/pyyaml (MIT License)
try:
    from .pyyaml.lib import yaml
except ImportError as e:
    raise ImportError(
        "\nERROR: Vendored PyYAML not found!\n"
        "Run: git submodule update --init --recursive\n"
    ) from e

__all__ = ['yaml']
```

**Why no fallback**: Fail fast to prevent dev/prod inconsistency.

### 3. Create Documentation

**File**: `src/vendored/README.md`

```markdown
# Vendored Dependencies

| Package | Version | License | Source |
|---------|---------|---------|--------|
| PyYAML  | 6.0.3   | MIT     | https://github.com/yaml/pyyaml |

## Usage
```python
from vendored import yaml
```

See CONTRIBUTING.md for vendoring workflow.
```

### 4. Update CMake

**Add submodule auto-init** (after `find_package(Python3)`, ~line 100):

```cmake
# Auto-initialize vendored dependencies
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/vendored/pyyaml/lib/yaml/__init__.py")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive src/vendored/pyyaml
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE GIT_SUBMOD_RESULT
        )
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update failed for src/vendored/pyyaml")
        endif()
    endif()
endif()
```

**Add installation** (after other `install(DIRECTORY src/...)`, ~line 722):

```cmake
# Vendored dependencies
install(FILES src/vendored/__init__.py
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/${PROJECT_NAME}/vendored
        COMPONENT main)

install(DIRECTORY src/vendored/pyyaml/lib/yaml
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/${PROJECT_NAME}/vendored/pyyaml/lib
        COMPONENT main
        FILES_MATCHING PATTERN "*.py"
        PATTERN "__pycache__" EXCLUDE
        PATTERN "*.pyc" EXCLUDE)
```

**Note**: The CMake pyyaml dependency check (lines 110-112) is NOT removed in Phase 3. It will be removed in Phase 5 after pyyaml is removed from requirements.txt.

### 5. Update Python Imports

Change in 3 files:
- `src/rocprof_compute_profile/profiler_base.py` (line 38)
- `src/rocprof_compute_soc/soc_base.py` (line 35)
- `src/utils/mi_gpu_spec.py` (line 29)

```python
# BEFORE:
import yaml

# AFTER:
from vendored import yaml
```

### 6. Requirements

**No changes needed**:
- `requirements.txt` already has `pyyaml==6.0.3` (still needed by analyze mode and utils.py)
- `requirements-test.txt` doesn't need it (tests use pyyaml from requirements.txt)

### 7. Update Documentation

**README.md** - Add to "Development from Source":
```markdown
### Prerequisites
git submodule update --init --recursive
```

**CONTRIBUTING.md** - Add "Vendoring External Dependencies" section after "Metrics Management":
- Vendoring criteria (pure Python, permissive license, profile code path)
- How to add packages (git submodule, CMake, imports)
- How to update packages
- SQLAlchemy example for future reference

### 8. Testing

Run existing profile tests:
- Verify YAML operations work with vendored PyYAML
- Confirm no regressions
- Test dev mode (source) and install mode

Optional: Add `tests/test_vendored.py` to verify profile uses vendored yaml.

---

## Success Criteria

- [ ] Git submodule added: `src/vendored/pyyaml/` (PyYAML 6.0.3)
- [ ] Files created: `src/vendored/__init__.py`, `src/vendored/README.md`
- [ ] CMake: Auto-init submodules, install vendored packages
- [ ] Imports updated (3 files): profiler_base.py, soc_base.py, mi_gpu_spec.py
- [ ] Requirements: No changes (pyyaml already in requirements.txt)
- [ ] Docs: README.md (submodule init), CONTRIBUTING.md (vendoring workflow)
- [ ] Tests pass, both dev and install modes work

---

## Files Modified

**New**:
- `src/vendored/__init__.py`
- `src/vendored/README.md`
- `.gitmodules`

**Modified**:
- `CMakeLists.txt` (submodule init, installation)
- `src/rocprof_compute_profile/profiler_base.py` (line 38)
- `src/rocprof_compute_soc/soc_base.py` (line 35)
- `src/utils/mi_gpu_spec.py` (line 29)
- `README.md`
- `CONTRIBUTING.md`

**Submodule**: `src/vendored/pyyaml/` (PyYAML 6.0.3)

**NOT Modified**:
- `src/utils/utils.py` (Phase 4 scope)
- `requirements.txt` (keeps pyyaml)
- `LICENSE.md` (already lists PyYAML line 36)
- `CHANGELOG.md` (internal refactor, transparent to users)

---

## Notes

- **Location**: `src/vendored/` follows Python convention (pip, setuptools use `_vendor/`)
- **Pure Python only**: No C extensions for portability
- **Phase boundary**: utils.py segregation is Phase 4's responsibility
- **Partial completion**: Profile still uses external PyYAML via utils.py until Phase 4
- **CMake auto-init**: Submodules initialized during configure (seamless for CI and builds)
- **No user-facing changes**: Internal refactoring only
