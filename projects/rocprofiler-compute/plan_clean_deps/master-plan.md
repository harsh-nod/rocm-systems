# MASTER PLAN: Eliminate Non-Standard Python Dependencies from rocprof-compute Profile Mode

**JIRA Epic**: AIPROFCOMP-29

## Executive Summary

This master plan outlines the strategy to refactor rocprofiler-compute so that the `profile` mode code path never imports non-standard Python dependencies. Each phase is designed as an independent, self-contained PR that includes ALL necessary code changes, tests, and documentation.

**Detailed phase plans**: See individual phase plan files in this directory.

---

## Current State

- Profile mode requires all 13 packages from `requirements.txt` at startup
- Only 2 non-standard packages actually used in profile: `pyyaml` (6.0.3) and `pandas` (2.2.3)
- 11 packages are analysis-only but checked at profile startup
- Build-time CMake checks enforce all dependencies before build completes
- Runtime `verify_deps()` checks all dependencies before any profiling begins

---

## Target State

- Profile mode imports ZERO non-standard Python dependencies
- All analysis dependencies (pandas, plotly, dash, textual, etc.) lazy-loaded only in analyze mode
- YAML handling via bundled lightweight library or pure Python implementation
- CSV operations using only stdlib (`csv`, `sqlite3`)
- Test suite to guarantee no non-standard imports in profile code path
- Dependencies only checked in analyze mode, never in profile or build

---

## Phase Overview and Shipping Sequence

Each PR is a **complete package**: code + tests + docs, all tests passing ✅

| Ship Order | Phase/PR | Theme | Duration | Dependencies | Plan File |
|------------|----------|-------|----------|--------------|-----------|
| **1st** | Phase 1 (PR #1) | Roofline HTML → Analyze Mode | 1-2 days | None | [phase-1-roofline-to-analyze.md](phase-1-roofline-to-analyze.md) |
| **2nd** | Phase 2 (PR #2) | Eliminate Pandas from Profile | 3-5 days | None | [phase-2-eliminate-pandas.md](phase-2-eliminate-pandas.md) |
| **3rd** | Phase 3 (PR #3) | Eliminate PyYAML from Profile | 3-5 days | PR #2 merged | [phase-3-eliminate-pyyaml.md](phase-3-eliminate-pyyaml.md) |
| **4th** | Phase 4 (PR #4) | Refactor utils.py (stdlib only) | 2-3 days | PR #2, #3 merged | [phase-4-refactor-utils.md](phase-4-refactor-utils.md) |
| **LAST** | Phase 5 (PR #5) | Test Infra + Remove Dep Checking | 1-2 days | All merged | [phase-5-test-and-dep-checking.md](phase-5-test-and-dep-checking.md) |

**Total Timeline**: 5 PRs over ~4-6 weeks

---

## Phase Details

### Phase 1 (PR #1): Roofline HTML → Analyze Mode
**Status**: LOW-HANGING FRUIT - Ship first

**Removes**: plotly, dash, dash-bootstrap-components, dash-svg, plotext, plotille

**Changes**:
- Profile mode: Only generates roofline.csv (no HTML)
- Analyze mode: Generates all visualizations
- Tests: Verify no HTML in profile output
- Docs: Update profiling workflow documentation

**Why First**:
- Quick win, immediate value
- Independent of other changes
- All tests pass

---

### Phase 2 (PR #2): Eliminate Pandas
**Status**: Move CSV joining to analyze mode

**Removes**: pandas

**Changes**:
- Move `join_prof()` method from profiler_base.py to analysis_base.py
- Move `detect_missing_counters()` helper to analyze mode
- Profile creates separate `pmc_perf_*.csv` files
- Analyze mode joins them into `pmc_perf.csv` (backward compatible)
- Tests: Update 30+ assertions expecting pmc_perf.csv after profile
- Tests: Add new test file for analyze join_prof functionality
- Docs: Update workflow (profile → analyze two-step)

**Why Second**:
- Can develop in parallel with Phase 1
- Independent implementation
- Largest dependency removed (~50MB)

---

### Phase 3 (PR #3): Eliminate PyYAML
**Status**: Depends on Phase 2

**Removes**: pyyaml (external)

**Changes**:
- Create minimal YAML library: `utils/yaml_lib.py` (~300-400 lines)
- Implement only `safe_load()` and `dump()` for basic types (dict/list/str/int/bool/None)
- Based on PyYAML patterns (attribution only, no LICENSE update)
- Update imports: `from utils import yaml_lib as yaml` (3 files)
- Handles 4 YAML usage points in profile:
  - profiler_base.py line 477 (profiling config)
  - soc_base.py lines 302-336 (analysis configs during detect_counters)
  - soc_base.py line 656 (counter definitions)
  - utils.py lines 918-933 (counter defs read/merge)
- Tests: Roundtrip validation, verify no external pyyaml import
- Docs: None required (internal implementation)

**Why Third**:
- Easier after pandas removed (fewer merge conflicts)
- **After this: Profile has ZERO external dependencies** 🎉

---

### Phase 4 (PR #4): Refactor utils.py
**Status**: Cleanup after stdlib conversion

**Removes**: N/A (organizational refactoring)

**Changes**:
- Split `utils/utils.py` (2251 lines) into:
  - `utils/utils_common.py` - Shared stdlib utilities
  - `utils/utils_profile.py` - Profile-specific (stdlib only)
  - `utils/utils_analysis.py` - Analysis-specific (heavy deps OK)
- Update all imports across codebase
- Tests: Verify no import errors, all tests pass
- Docs: Document new module structure

**Why Fourth**:
- Consolidates stdlib-only utilities after conversion
- Clean module boundaries

---

### Phase 5 (PR #5): Test Infrastructure + Remove Dep Checking
**Status**: Validation and final cleanup - SHIPS LAST

**Adds**: Import guard to protect ALL profile tests

**Changes**:
- Add `ProfileModeImportGuard` class to `tests/conftest.py` (~100 lines)
- Guard `binary_handler_profile_rocprof_compute` fixture (auto-protects ALL profile tests)
- Uses `sys.meta_path` import hook (fast, elegant, PEP 302)
- DELETE `CHECK_PYTHON_DEPS` from CMakeLists.txt entirely (not make optional)
- Move `verify_deps()` call to analyze mode only (remove from profile startup)
- Update `verify_deps()` error message to guide users to analyze-only
- Tests: **All profile tests PASS** ✅ (guard validates zero deps)
- Docs: Add dependency policy to CONTRIBUTING.md (~150 lines)

**Why Last**:
- Tests only pass after ALL dependencies eliminated
- Reviewers see GREEN tests, not RED
- Validates entire refactoring effort
- Serves as regression guard going forward (can't bypass fixture)

---

## Development Timeline

### Week 1-2: Phase 1 + Start Phase 2
- **Ship PR #1** (Roofline) immediately
- Develop PR #2 (Pandas) in parallel

### Week 3-4: Phase 2 + Start Phase 3
- **Ship PR #2** (Pandas)
- Develop PR #3 (PyYAML) in parallel branch

### Week 5: Phase 3 + Start Phase 4
- **Ship PR #3** (PyYAML)
- **Profile mode now has ZERO dependencies** 🎉
- Develop PR #4 (Refactor utils)

### Week 6: Phase 4 + Phase 5
- **Ship PR #4** (Refactor utils)
- Develop PR #5 (Test + Dep Checking)
- **Ship PR #5** (LAST)
- **Test passes, validates zero dependencies** ✅

---

## Success Criteria

**After Phase 1**:
- ✅ Roofline HTML only in analyze mode
- ✅ 6 visualization packages removed from profile path

**After Phase 2**:
- ✅ Pandas removed from profile
- ✅ CSV operations use stdlib only

**After Phase 3**:
- ✅ PyYAML removed from profile (or bundled)
- ✅ **Profile mode has ZERO external Python dependencies**

**After Phase 4**:
- ✅ Clean module structure
- ✅ Stdlib-only utils clearly separated

**After Phase 5**:
- ✅ Import guard protects ALL profile tests (automatic via fixture)
- ✅ CMake doesn't check Python dependencies (deleted entirely)
- ✅ Dependencies only checked in analyze mode (not profile startup)
- ✅ Profile runs on any Python 3.8+ system (no pip install)
- ✅ All tests PASS (validates Phases 1-4 success)

---

## Final Validation

```bash
# On fresh Python 3.8+ system with NO packages installed:
python3 src/rocprof-compute profile --roof-only -- /bin/true
# ✅ Should complete successfully without ANY pip install commands

# Test suite validates (guard auto-runs on ALL profile tests):
pytest tests/test_profile_general.py -v
# ✅ PASSES - import guard in fixture ensures no non-stdlib imports

# CMake doesn't check Python deps:
rm -rf build && mkdir build && cd build && cmake ..
# ✅ Should work with bare Python 3.8+ (no pip install required)

# After installing analyze dependencies:
pip install -r requirements.txt
python3 src/rocprof-compute analyze -p <workload_dir>
# ✅ Should generate full analysis with visualizations
```

---

## Key Principles

1. **Every PR ships with all tests passing** - No red CI
2. **Every PR is complete** - Code + tests + docs included
3. **Every PR is reviewable** - Clear scope, focused changes
4. **Phases are independent where possible** - Parallel development
5. **Test ships last** - Only when it will pass

---

## Critical Files Reference

### Profile Mode Execution Path (must be stdlib-only)

**Entry Point**:
- `src/rocprof-compute` - Main entry script

**Core Profile Modules**:
- `src/rocprof_compute_base.py` (612 lines)
- `src/rocprof_compute_profile/profiler_base.py` (826 lines)
- `src/rocprof_compute_profile/profiler_rocprof_v3.py` (139 lines)
- `src/rocprof_compute_profile/profiler_rocprofiler_sdk.py` (168 lines)

**Utility Modules**:
- `src/utils/utils.py` (2251 lines) → Will be split in Phase 4
- `src/utils/logger.py` (stdlib only)

**SoC Modules**:
- `src/rocprof_compute_soc/soc_base.py` (806 lines)
- `src/rocprof_compute_soc/soc_*.py` (architecture-specific)

### Roofline-Related
- `src/roofline.py` - Used in both modes (Phase 1 changes)

---

## Risk Mitigation

### Risk: CSV joining moved to analyze increases workflow steps
**Mitigation**: Backward compatible - analyze detects and joins automatically. Users gain flexibility (can re-analyze without re-profiling)

### Risk: Minimal YAML library breaks existing configs
**Mitigation**: Based on PyYAML patterns, handles only basic types used in our configs. Comprehensive roundtrip tests validate correctness

### Risk: Breaking changes affect users
**Mitigation**: Maintain backward compatibility, document migration

### Risk: Standalone binary breaks
**Mitigation**: Test binary in CI for each phase

---

## Questions for BU

1. **Roofline workflow change** (Phase 1): OK to require separate analyze step for HTML?
   - Answer: Yes (proceed with two-step workflow)
2. **YAML approach** (Phase 3): Minimal library vs vendoring full PyYAML?
   - Answer: Minimal library preferred (simpler, ~300 lines vs 5000+ lines)
3. **Timeline**: ~6 weeks acceptable? Can some PRs be fast-tracked?
   - Answer: TBD

---

## Next Steps

1. Review and approve this master plan
2. Start with Phase 1 (Roofline HTML) - low-hanging fruit
3. Develop Phase 2 (Pandas) in parallel
4. Sequential shipping as phases complete
5. Final validation with Phase 5 (test ships green)

This plan ensures rocprof-compute profile mode achieves **zero non-standard Python dependencies** while maintaining all functionality in analyze mode.
