////////////////////////////////////////////////////////////////////////////////
// Unit tests for COMGR hotswap dataflow analysis infrastructure.
// Tests DefUse, CFG, Liveness, and ScratchAllocator via C-linkage entry points
// exposed in libamd_comgr.so (loaded via dlopen).
////////////////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

static const char *kCPU = "gfx1250";

// ── Function pointer types for COMGR test entry points ───────────────────────

using DefUseFn = int (*)(const char *asm_text, const char *cpu,
                         int *out_defs, int *out_def_count,
                         int *out_uses, int *out_use_count, int max_regs);

using CfgFn = int (*)(const char **asm_lines, int num_lines, const char *cpu,
                       uint64_t *out_bb_starts, int *out_bb_succ_counts,
                       int max_blocks);

using LivenessFn = int (*)(const char **asm_lines, int num_lines,
                           const char *cpu, int inst_index,
                           int *out_live, int max_regs);

using ScratchAllocFn = int (*)(const int *live_vgprs, int num_live,
                               int kd_allocated_vgprs);

static DefUseFn g_defuse;
static CfgFn g_cfg;
static LivenessFn g_liveness;
static ScratchAllocFn g_scratch_alloc;

// ── Test infrastructure ──────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

#define CHECK(cond, msg) do { \
  if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s\n        %s:%d\n", msg, __FILE__, __LINE__); \
    g_failed++; \
    return; \
  } \
} while (0)

#define CHECK_EQ(actual, expected, msg) do { \
  auto _a = (actual); auto _e = (expected); \
  if (_a != _e) { \
    fprintf(stderr, "  FAIL: %s (expected %d, got %d)\n        %s:%d\n", \
            msg, (int)_e, (int)_a, __FILE__, __LINE__); \
    g_failed++; \
    return; \
  } \
} while (0)

static void Pass(const char *name) {
  fprintf(stderr, "  PASS: %s\n", name);
  g_passed++;
}

static bool SetContains(const int *arr, int count, int val) {
  for (int i = 0; i < count; i++)
    if (arr[i] == val) return true;
  return false;
}

static std::set<int> ToSet(const int *arr, int count) {
  std::set<int> s;
  for (int i = 0; i < count; i++) s.insert(arr[i]);
  return s;
}

// ── DefUse Tests (Milestone 1) ───────────────────────────────────────────────

static void TestDefUse_SingleVGPR() {
  int defs[64], uses[64], dc = 0, uc = 0;
  int rc = g_defuse("v_mov_b32 v5, v10", kCPU, defs, &dc, uses, &uc, 64);
  CHECK(rc >= 0, "defuse call should succeed for v_mov_b32");
  CHECK(dc >= 1, "v_mov_b32 v5, v10: should define at least 1 VGPR");
  CHECK(SetContains(defs, dc, 5), "v_mov_b32 v5, v10: defs should contain v5");
  CHECK(uc >= 1, "v_mov_b32 v5, v10: should use at least 1 VGPR");
  CHECK(SetContains(uses, uc, 10), "v_mov_b32 v5, v10: uses should contain v10");
  Pass("TestDefUse_SingleVGPR");
}

static void TestDefUse_TupleWrite() {
  int defs[64], uses[64], dc = 0, uc = 0;
  int rc = g_defuse(
      "v_wmma_f32_16x16x64_fp8_fp8 v[0:7], v[8:15], v[16:23], v[0:7]",
      kCPU, defs, &dc, uses, &uc, 64);
  CHECK(rc >= 0, "defuse call should succeed for v_wmma");
  auto dset = ToSet(defs, dc);
  for (int v = 0; v <= 7; v++) {
    char msg[128];
    snprintf(msg, sizeof(msg), "wmma defs should contain v%d", v);
    CHECK(dset.count(v), msg);
  }
  auto uset = ToSet(uses, uc);
  for (int v = 0; v <= 23; v++) {
    char msg[128];
    snprintf(msg, sizeof(msg), "wmma uses should contain v%d", v);
    CHECK(uset.count(v), msg);
  }
  Pass("TestDefUse_TupleWrite");
}

static void TestDefUse_SGPRIgnored() {
  int defs[64], uses[64], dc = 0, uc = 0;
  int rc = g_defuse("s_add_u32 s0, s1, s2", kCPU, defs, &dc, uses, &uc, 64);
  CHECK(rc >= 0, "defuse call should succeed for s_add_u32");
  CHECK_EQ(dc, 0, "s_add_u32: should have 0 VGPR defs");
  CHECK_EQ(uc, 0, "s_add_u32: should have 0 VGPR uses");
  Pass("TestDefUse_SGPRIgnored");
}

static void TestDefUse_MixedOperands() {
  int defs[64], uses[64], dc = 0, uc = 0;
  int rc = g_defuse("global_load_b32 v5, v[10:11], off",
                    kCPU, defs, &dc, uses, &uc, 64);
  CHECK(rc >= 0, "defuse call should succeed for global_load_b32");
  CHECK(SetContains(defs, dc, 5),
        "global_load_b32: defs should contain v5");
  auto uset = ToSet(uses, uc);
  CHECK(uset.count(10), "global_load_b32: uses should contain v10");
  CHECK(uset.count(11), "global_load_b32: uses should contain v11");
  Pass("TestDefUse_MixedOperands");
}

static void TestDefUse_BranchNoVGPR() {
  int defs[64], uses[64], dc = 0, uc = 0;
  int rc = g_defuse("s_branch 0", kCPU, defs, &dc, uses, &uc, 64);
  CHECK(rc >= 0, "defuse call should succeed for s_branch");
  CHECK_EQ(dc, 0, "s_branch: should have 0 VGPR defs");
  CHECK_EQ(uc, 0, "s_branch: should have 0 VGPR uses");
  Pass("TestDefUse_BranchNoVGPR");
}

// ── CFG Tests (Milestone 2) ──────────────────────────────────────────────────

static void TestCFG_Linear() {
  const char *lines[] = {"v_mov_b32 v0, v1", "v_mov_b32 v2, v3", "s_endpgm"};
  uint64_t starts[16];
  int succs[16];
  int n = g_cfg(lines, 3, kCPU, starts, succs, 16);
  CHECK(n >= 1, "linear program should have at least 1 basic block");
  CHECK_EQ(n, 1, "linear program should have exactly 1 basic block");
  CHECK_EQ(succs[0], 0, "single block should have 0 successors (s_endpgm)");
  Pass("TestCFG_Linear");
}

static void TestCFG_UnconditionalBranch() {
  // s_branch 0 jumps to offset (current + 4 + 0*4) = current+4, which is
  // the next instruction. But it still creates a block split.
  const char *lines[] = {"v_mov_b32 v0, v1", "s_branch 0",
                         "v_mov_b32 v2, v3", "s_endpgm"};
  uint64_t starts[16];
  int succs[16];
  int n = g_cfg(lines, 4, kCPU, starts, succs, 16);
  CHECK(n >= 2, "unconditional branch should create at least 2 basic blocks");
  Pass("TestCFG_UnconditionalBranch");
}

static void TestCFG_ConditionalBranch() {
  // s_cbranch_scc0 with offset 0 targets next instruction's offset + 0*4
  const char *lines[] = {"v_mov_b32 v0, v1", "s_cbranch_scc0 0",
                         "v_mov_b32 v2, v3", "s_endpgm"};
  uint64_t starts[16];
  int succs[16];
  int n = g_cfg(lines, 4, kCPU, starts, succs, 16);
  CHECK(n >= 2, "conditional branch should create at least 2 basic blocks");
  Pass("TestCFG_ConditionalBranch");
}

// ── Liveness Tests (Milestone 3) ─────────────────────────────────────────────

static void TestLiveness_LinearDead() {
  // v5 is defined at inst 0 but never used before s_endpgm
  const char *lines[] = {"v_mov_b32 v5, v0", "s_endpgm"};
  int live[256];
  int n = g_liveness(lines, 2, kCPU, 1, live, 256);
  CHECK(n >= 0, "liveness call should succeed");
  auto lset = ToSet(live, n);
  CHECK(!lset.count(5), "v5 should be dead at s_endpgm (never used after def)");
  Pass("TestLiveness_LinearDead");
}

static void TestLiveness_LinearLive() {
  // v5 is defined at inst 0, used at inst 1 -> live before inst 1
  const char *lines[] = {"v_mov_b32 v5, v0", "v_add_f32 v6, v5, v5",
                         "s_endpgm"};
  int live[256];
  int n = g_liveness(lines, 3, kCPU, 1, live, 256);
  CHECK(n >= 0, "liveness call should succeed");
  auto lset = ToSet(live, n);
  CHECK(lset.count(5), "v5 should be live before v_add_f32 (used as source)");
  Pass("TestLiveness_LinearLive");
}

static void TestLiveness_PostEndpgm() {
  // After s_endpgm, nothing should be live
  const char *lines[] = {"v_mov_b32 v5, v0", "s_endpgm", "s_nop 0"};
  int live[256];
  int n = g_liveness(lines, 3, kCPU, 2, live, 256);
  CHECK(n >= 0, "liveness call should succeed");
  CHECK_EQ(n, 0, "nothing should be live after s_endpgm");
  Pass("TestLiveness_PostEndpgm");
}

static void TestLiveness_HighWatermark() {
  // Only v0-v3 are used, v4+ should be dead
  const char *lines[] = {"v_mov_b32 v0, v1", "v_mov_b32 v2, v3", "s_endpgm"};
  int live[256];
  int n = g_liveness(lines, 3, kCPU, 0, live, 256);
  CHECK(n >= 0, "liveness call should succeed");
  auto lset = ToSet(live, n);
  CHECK(!lset.count(4), "v4 should be dead in low-pressure kernel");
  CHECK(!lset.count(10), "v10 should be dead in low-pressure kernel");
  CHECK(!lset.count(31), "v31 should be dead in low-pressure kernel");
  Pass("TestLiveness_HighWatermark");
}

// ── ScratchAllocator Tests (Milestone 4) ──────────────────────────────────────

static void TestAlloc_DeadRegReuse() {
  // v0-v30 are live, kd=32 -> v31 is dead within range, should be returned
  std::vector<int> live;
  for (int i = 0; i < 31; i++) live.push_back(i);
  int r = g_scratch_alloc(live.data(), live.size(), 32);
  CHECK_EQ(r, 31, "should reuse dead v31 within KD range");
  Pass("TestAlloc_DeadRegReuse");
}

static void TestAlloc_AboveKD() {
  // All of kd range (0-63) is live -> must allocate above
  std::vector<int> live;
  for (int i = 0; i < 64; i++) live.push_back(i);
  int r = g_scratch_alloc(live.data(), live.size(), 64);
  CHECK_EQ(r, 64, "should allocate v64 above KD range");
  Pass("TestAlloc_AboveKD");
}

static void TestAlloc_Exhausted() {
  // All 256 VGPRs live -> allocation fails
  std::vector<int> live;
  for (int i = 0; i < 256; i++) live.push_back(i);
  int r = g_scratch_alloc(live.data(), live.size(), 256);
  CHECK_EQ(r, -1, "should return -1 when all VGPRs exhausted");
  Pass("TestAlloc_Exhausted");
}

static void TestAlloc_PreferHighest() {
  // live={0,1,2,4,5}, kd=8 -> dead within range: 3,6,7
  // allocator scans from top down, should pick 7
  int live[] = {0, 1, 2, 4, 5};
  int r = g_scratch_alloc(live, 5, 8);
  CHECK_EQ(r, 7, "should prefer highest dead VGPR in KD range");
  Pass("TestAlloc_PreferHighest");
}

// ── Integration Test ─────────────────────────────────────────────────────────

static void TestIntegration_LivenessAllocCombined() {
  // Build a small program, run liveness, then allocate from the live set
  const char *lines[] = {
      "v_mov_b32 v0, v1",      // def v0, use v1
      "v_add_f32 v2, v0, v1",  // def v2, use v0, v1
      "v_mul_f32 v3, v2, v0",  // def v3, use v2, v0
      "s_endpgm"
  };

  // Get liveness at inst 2 (v_mul_f32)
  int live_arr[256];
  int n = g_liveness(lines, 4, kCPU, 2, live_arr, 256);
  CHECK(n >= 0, "integration liveness call should succeed");
  auto lset = ToSet(live_arr, n);
  CHECK(lset.count(0), "v0 should be live at inst 2 (used by v_mul)");
  CHECK(lset.count(2), "v2 should be live at inst 2 (used by v_mul)");

  // Now use the live set to allocate a scratch register with kd=8
  int r = g_scratch_alloc(live_arr, n, 8);
  CHECK(r >= 0, "scratch alloc should succeed with headroom");
  CHECK(!lset.count(r), "allocated register should not be in live set");
  Pass("TestIntegration_LivenessAllocCombined");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
  const char *lib_path = getenv("HSA_HOTSWAP_COMGR_LIB");
  if (!lib_path) {
    fprintf(stderr, "HSA_HOTSWAP_COMGR_LIB not set, skipping dataflow tests\n");
    return 0;
  }

  void *handle = dlopen(lib_path, RTLD_LAZY);
  if (!handle) {
    fprintf(stderr, "Failed to dlopen %s: %s\n", lib_path, dlerror());
    fprintf(stderr, "Skipping dataflow tests\n");
    return 0;
  }

  g_defuse = (DefUseFn)dlsym(handle, "amd_comgr_test_defuse");
  g_cfg = (CfgFn)dlsym(handle, "amd_comgr_test_cfg");
  g_liveness = (LivenessFn)dlsym(handle, "amd_comgr_test_liveness");
  g_scratch_alloc = (ScratchAllocFn)dlsym(handle, "amd_comgr_test_scratch_alloc");

  if (!g_defuse || !g_cfg || !g_liveness || !g_scratch_alloc) {
    fprintf(stderr, "Failed to resolve test symbols:\n");
    if (!g_defuse) fprintf(stderr, "  missing: amd_comgr_test_defuse\n");
    if (!g_cfg) fprintf(stderr, "  missing: amd_comgr_test_cfg\n");
    if (!g_liveness) fprintf(stderr, "  missing: amd_comgr_test_liveness\n");
    if (!g_scratch_alloc)
      fprintf(stderr, "  missing: amd_comgr_test_scratch_alloc\n");
    dlclose(handle);
    return 1;
  }

  fprintf(stderr, "=== Dataflow Analysis Tests ===\n");

  fprintf(stderr, "\n--- DefUse Tests ---\n");
  TestDefUse_SingleVGPR();
  TestDefUse_TupleWrite();
  TestDefUse_SGPRIgnored();
  TestDefUse_MixedOperands();
  TestDefUse_BranchNoVGPR();

  fprintf(stderr, "\n--- CFG Tests ---\n");
  TestCFG_Linear();
  TestCFG_UnconditionalBranch();
  TestCFG_ConditionalBranch();

  fprintf(stderr, "\n--- Liveness Tests ---\n");
  TestLiveness_LinearDead();
  TestLiveness_LinearLive();
  TestLiveness_PostEndpgm();
  TestLiveness_HighWatermark();

  fprintf(stderr, "\n--- ScratchAllocator Tests ---\n");
  TestAlloc_DeadRegReuse();
  TestAlloc_AboveKD();
  TestAlloc_Exhausted();
  TestAlloc_PreferHighest();

  fprintf(stderr, "\n--- Integration Tests ---\n");
  TestIntegration_LivenessAllocCombined();

  fprintf(stderr, "\n=== Results: %d passed, %d failed ===\n",
          g_passed, g_failed);

  dlclose(handle);
  return g_failed > 0 ? 1 : 0;
}
