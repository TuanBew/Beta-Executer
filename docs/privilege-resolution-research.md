# Privilege Resolution Research — ScriptContext Discovery

**Date:** 2026-08-02
**Branch:** `refactor/privilege-elevation-v2`
**Status:** Implementation complete, pending runtime verification

---

## Problem Statement

The privilege elevation pipeline fails to reach level 10 because it cannot locate the real ScriptContext object. Both resolution paths return level 0.

## Root Cause: MSVC x64 Small String Optimization (SSO) Bug

### Background

The target application uses MSVC x64 `std::string` for job names. MSVC's `std::string` layout on x64 is:

```
Offset  Field        Size   Description
+0x00   union        16     Either inline char buffer (SSO) or heap pointer
+0x10   _Mysize       8     String length
+0x18   _Myres        8     Buffer capacity (SSO threshold: < 16)
```

When `capacity < 16`, the string data is stored **inline** in the first 16 bytes — no pointer indirection. When `capacity >= 16`, a heap pointer occupies the first 8 bytes.

### The Bug

Path B reads job names with:
```cpp
uintptr_t namePtr = Memory::Read<uintptr_t>(jobPtr + 0x18);  // offset Job_Name
if (IsValidPointer(namePtr))
    jobName = Memory::ReadString(namePtr, 64);
```

For a job named "RenderJob" (9 chars, SSO-stored):
- The first 8 bytes at `jobPtr + 0x18` contain ASCII: `R e n d e r J o`
- Interpreted as `uint64_t` (little-endian): `0x6F4A7265646E6552`
- `IsValidPointer` checks range `0x10000..0x7FFFFFFFFFFF`
- `0x6F4A7265646E6552 > 0x7FFFFFFFFFFF` → **fails validation** → name stays empty
- The RenderJob is never matched, Path B returns 0

This affects ALL jobs with names ≤ 15 characters. Since most standard job names are short, **every single job name read fails silently**, explaining why 144 entries yield 0 matches.

### The Fix

```cpp
static std::string ReadMSVCString(uintptr_t stringAddr) {
    size_t capacity = Memory::Read<size_t>(stringAddr + 0x18);
    size_t length   = Memory::Read<size_t>(stringAddr + 0x10);
    if (length == 0 || length > 4096) return "";

    if (capacity < 16) {
        // SSO: read inline from stringAddr directly
        auto bytes = Memory::ReadBytes(stringAddr, length);
        return std::string(reinterpret_cast<const char*>(bytes.data()), length);
    }

    // Heap-allocated: follow pointer
    uintptr_t ptr = Memory::Read<uintptr_t>(stringAddr);
    return Memory::ReadString(ptr, length);
}
```

The `NameSize = 0x10` offset in `offsets.h` independently confirms this layout.

---

## Path A: Proxy DataModel Issue

Path A resolves via: `moduleBase + 0x7E26978 → FakeDataModel + 0x1D0 → DataModel`

The DataModel found at `FakeDataModel + 0x1D0` passes ClassName validation ("DataModel") and has a valid Children pointer, but:
- `DataModel + 0x440` (ScriptContext offset) reads null
- Children walk reports "empty or reversed" (childEnd <= childStart)

**Diagnosis:** This is a proxy/shell DataModel — it has correct class metadata but doesn't host the real object tree. The self-referential pointer pattern (DataModel only 0x1C8 bytes from FakeDataModel) suggests it's part of the FakeDataModel allocation.

**Fix applied:** Reject empty ClassDescriptor names (was passing them through), add proxy detection warning logging.

---

## Resolution Paths Implemented (v2.1)

### Path A — FakeDataModel chain
```
moduleBase + 0x7E26978 → FakeDataModel → +0x1D0 → DataModel
```
- Validation: Children pointer + ClassName must contain "DataModel"
- Proxy detection: warns if ScriptContext null AND children empty
- **Likely returns proxy — fallback to Path B**

### Path B — TaskScheduler/RenderJob (SSO-fixed)
```
moduleBase + 0x84A58E0 → TaskScheduler → +0xC8/+0xD0 → job array
  → iterate all jobs, SSO-aware name read
  → match "render" (case-insensitive)
  → RenderJob + 0x1C8 → DataModel
```
- **Expected primary path** after SSO fix
- SSO-aware: reads capacity at `+0x18`, inline vs pointer accordingly
- Case-insensitive matching via `std::transform` to lowercase

### Path C — VisualEngine (exploratory)
```
moduleBase + 0x8818F60 → container → +0x10 → VisualEngine object
  → probe offsets 0x1C8, 0x1D0, 0x38, 0x28, 0x30, 0x40, 0x48
  → validate each via ClassName containing "DataModel"
```
- Backup route if both A and B fail
- Probes multiple known offsets for DataModel references

### ScriptContext Resolution (from any DataModel)
```
DataModel + 0x440 → ScriptContext (direct)
  OR
DataModel + 0x70/+0x78 → Children array → iterate → match ClassName == "ScriptContext"
```

---

## Elevation Strategies (unchanged from v2)

| # | Strategy | Target | Method |
|---|----------|--------|--------|
| 1+2 | Direct identity write | `ScriptContext + 0x2C0` | Write level 10, also write to ActiveScript at `+0x2B0 → +0x1C8` |
| 3 | RequireBypass flag | `ScriptContext + 0x0` | **SKIPPED** — offset 0x0 overlaps vtable pointer, writing corrupts object |
| 4 | Identity-check detour | `ScriptContext + 0x2D0` | Patch function prologue to `MOV EAX, 10; RET` |

**Strategy 3 safety note:** `ScriptContextRequireBypass = 0x0` is at the very start of the object, which on x64 C++ objects is the vtable pointer. Writing a byte there corrupts virtual dispatch. The code now skips this strategy with a logged warning.

---

## Available Global Pointers (from offsets.h)

| Pointer | Offset | Status |
|---------|--------|--------|
| FakeDataModelPointer | `0x7E26978` | Used (Path A) — yields proxy DataModel |
| TaskSchedulerPointer | `0x84A58E0` | Used (Path B) — SSO bug now fixed |
| VisualEnginePointer | `0x8818F60` | Used (Path C) — exploratory probing |
| PlatformStatePointer | `0xB9FE4B31` | Unused — very large offset, likely unreliable |
| PlayerConfigurerPointer | `0x0` | Null — unusable |

---

## Diagnostics Capabilities (v2.1)

### RunDiagnostics() function
Outputs to logger:
1. Hex dump of FakeDataModel (256 bytes)
2. Hex dump of Path A DataModel candidate (256 bytes)
3. Hex dump of TaskScheduler (256 bytes)
4. First 20 jobs: raw bytes, SSO classification, decoded name, class name
5. VisualEngine hex dump (256 bytes)

### LuaBridge functions
- `run_diagnostics()` — triggers full diagnostic dump
- `enumerate_jobs()` — returns Lua table of `{address, name, className}` for all jobs
- `dump_memory(addr, count)` — returns hex string of N bytes at address
- `resolve_context()` — enriched with `fakeDataModel`, `candidateCount`, `pathATried/B/C`

---

## Verification Plan

1. Build: `cmake --build build --config Release`
2. Attach to target, click "Run Diagnostics" in Injector tab
3. Inspect logger for job names — confirm RenderJob is now readable
4. Verify Path B finds RenderJob → real DataModel → ScriptContext
5. Confirm level 10 via `get_privilege_level()` or GUI display
6. Run `level_check.lua` for automated verification

---

## Files Modified (v2.1 changes only)

| File | Changes |
|------|---------|
| `src/Core/Memory.h` | Added `ReadBytes()` bulk read |
| `src/Core/PrivilegeElevation.h` | Added `RunDiagnostics()`, `ResolveDataModelPathC()`, extended `ContextInfo` |
| `src/Core/PrivilegeElevation.cpp` | SSO fix, Path C, validation hardening, diagnostics, vtable guard |
| `src/Lua/LuaBridge.cpp` | Added `dump_memory`, `enumerate_jobs`, `run_diagnostics`, enriched `resolve_context` |
| `src/GUI/GUI.cpp` | Live privilege display in toolbar, "Run Diagnostics" button, neutral process name |
| `scripts/level_check.lua` | Extended with diagnostics, job enumeration, post-elevation re-check |
