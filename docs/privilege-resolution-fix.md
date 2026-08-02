# Privilege Resolution Fix — Research & Implementation Plan

**Date:** 2026-08-02
**Branch:** `refactor/privilege-elevation-v2`
**Status:** Implemented, awaiting runtime hex dump verification

---

## 1. Problem Summary

Privilege elevation fails at level 0 due to three independent bugs:

| Bug | Location | Effect |
|-----|----------|--------|
| Path B: no ClassName validation | `PrivilegeElevation.cpp:251` | Accepts any pointer at RenderJob+0x1C8 as DataModel |
| Children walk: linked-list assumption | `LuaBridge.cpp:561-589` | Treats pointer array as linked list, chases wild pointers |
| No TaskQueue route | N/A (missing) | Fails to use ScriptContextTaskQueue → ScriptContext direct path |

## 2. Root Cause Analysis

### 2.1 Path A — Proxy DataModel
```
moduleBase + 0x7E26978 → FakeDataModel → +0x1D0 → DataModel
```
- ClassName validates as "DataModel" (correct)
- `DataModel + 0x440` (ScriptContext) is null
- Children array is empty/reversed (childEnd <= childStart)
- **Diagnosis:** Shell proxy DataModel, not the real tree root

### 2.2 Path B — Unvalidated RenderJob candidate
```
moduleBase + 0x84A58E0 → TaskScheduler → RenderJob → +0x1C8 → ???
```
- SSO fix (v2.1) enables finding RenderJob by name
- `RenderJob + 0x1C8` returned a pointer that was accepted without ClassName check
- The candidate's children pointers contained ASCII text (`0x32336E69572D7374`)
- **Diagnosis:** Offset 0x1C8 may point to a non-DataModel object, or the DataModel is partially initialized

### 2.3 Children Walk — Array vs. Linked List
```cpp
// BUG: treats 0x8 as "next sibling" offset within child object
current = Memory::Read<uintptr_t>(current + offsets::ChildrenEnd); // +0x8 in child
```
- Target stores children as `std::vector<Instance*>` — contiguous pointer array
- Layout: `[start_ptr @ obj+0x70][end_ptr @ obj+0x78]`
- The Lua bridge was reading child+0x8 as a "next" pointer, actually reading into the child object itself

### 2.4 ChildrenEnd Offset Semantics
```cpp
inline constexpr uintptr_t ChildrenEnd = 0x8;
```
This is a **relative offset from Children**, meaning `Children + ChildrenEnd = 0x70 + 0x8 = 0x78`. The C++ code in PrivilegeElevation.cpp uses this correctly:
```cpp
uintptr_t childEnd = Memory::Read<uintptr_t>(dm + offsets::Children + offsets::ChildrenEnd);
```
But the Lua bridge used `current + offsets::ChildrenEnd` where `current` was a child address, not the parent.

## 3. Fixes Implemented

### 3.1 Path B Validation (PrivilegeElevation.cpp)
Added to `ResolveDataModelPathB`:
- ClassName check: candidate must contain "DataModel"
- Children plausibility: start and end must be valid pointers, end > start
- On failure: `continue` to try next RenderJob instead of returning 0

### 3.2 Route D — ScriptContextTaskQueue Direct Path
New function `ResolveScriptContextViaTaskQueue`:
- Scans TaskScheduler jobs for name containing "scriptcontext" (case-insensitive)
- Probes job object at 14 known offsets for a pointer whose ClassName == "ScriptContext"
- Returns ScriptContext directly, bypassing DataModel entirely
- Integrated into `ResolveContext` as first fallback when no DataModel candidates exist, and as last resort when all candidates fail ScriptContext resolution

### 3.3 Route E — Parent-Chain Cross-Validation
New function `ResolveDataModelViaParentWalk`:
- Takes a DataModel candidate, reads its first valid child
- Walks `child → Parent → Parent → ...` until ClassName == "DataModel"
- If the parent-chain root differs from the candidate, adds it as a new candidate
- Detects proxy DataModels that have children pointing to the real tree

### 3.4 Lua Bridge Children Fix
- `get_object_children`: reads `[start @ +0x70, end @ +0x78]` as pointer array
- `find_first_child`: same array-based iteration, SSO-aware name reading
- `get_remote_events`: same fix applied

### 3.5 HexDump Utility
- `Memory::HexDump(address, size)` in Memory.h returns formatted hex string
- Standard 16-byte-per-line format with address, hex, ASCII columns
- Used by PrivilegeElevation.cpp diagnostics and Lua bridge `dump_memory`

### 3.6 Enhanced Diagnostics
- `RunDiagnostics` now scans for ScriptContextTaskQueue jobs and probes their internal pointers
- Route D diagnostic: dumps ScriptContext and its Parent chain
- All hex dumps use Memory::HexDump for consistent output

## 4. Resolution Strategy (Post-Fix)

Resolution order in `ResolveContext`:
1. **Path A** → FakeDataModel chain (often yields proxy)
2. **Path B** → TaskScheduler/RenderJob chain (now with ClassName + Children validation)
3. **Path C** → VisualEngine probing
4. **Route E** → Parent-chain walk on any candidate with valid children
5. **Route D** → ScriptContextTaskQueue direct path (bypasses DataModel)

ScriptContext resolution per candidate:
1. Direct offset: `DataModel + 0x440`
2. Children walk: iterate `DataModel.Children` for ClassName == "ScriptContext"

## 5. Verification Plan

### Step 1: Build
```bash
cmake --build build --config Release
```

### Step 2: Run research_dump.lua
Attach to target, execute `scripts/research_dump.lua`. This produces `docs/memory-dump.txt` with hex dumps of all critical objects.

### Step 3: Analyze Hex Dumps
From the dump file, verify:
- [ ] Children array at DataModel+0x70/+0x78: are these valid user-mode pointers with end > start?
- [ ] ScriptContextTaskQueue job: which offset holds the ScriptContext pointer?
- [ ] RenderJob+0x1C8: does it actually point to a DataModel (check ClassName bytes)?
- [ ] ScriptContext+0x2C0: current identity level value

### Step 4: Confirm Elevation
Run `scripts/level_check.lua`. Expected output:
```
Validated DataModel via [route] at 0x...
ScriptContext found via [method] at 0x...
Elevation successful: effective level 10
```

## 6. Hex Dump Data (Placeholder)

> Run `research_dump.lua` to populate `docs/memory-dump.txt`, then paste key findings here.

### DataModel Candidate Analysis
*(to be filled after first run)*

### ScriptContextTaskQueue Probe Results
*(to be filled after first run)*

### Children Array Offset Verification
*(to be filled after first run)*

## 7. Files Modified

| File | Changes |
|------|---------|
| `src/Core/Memory.h` | Added `HexDump()` utility function |
| `src/Core/PrivilegeElevation.h` | Added Route D/E documentation comments |
| `src/Core/PrivilegeElevation.cpp` | Path B validation, Route D, Route E, enhanced diagnostics, HexDump integration |
| `src/Lua/LuaBridge.cpp` | Fixed children walk (array vs linked list), SSO-aware find_first_child, updated dump_memory |
| `scripts/research_dump.lua` | New: comprehensive hex dump collector script |
| `scripts/level_check.lua` | Updated: new route reporting, hex dump support |
| `docs/privilege-resolution-fix.md` | This document |

## 8. Offsets Reference

| Symbol | Offset | Usage |
|--------|--------|-------|
| Children (start) | `obj + 0x70` | Pointer to first element of children array |
| Children (end) | `obj + 0x78` | Pointer past last element of children array |
| Parent | `obj + 0x68` | Pointer to parent Instance |
| ClassDescriptor | `obj + 0x18` | Pointer to class metadata |
| ClassDescriptorToClassName | `cd + 0x8` | Pointer to class name string |
| Name (MSVC string) | `obj + 0x98` | SSO-aware std::string (inline if cap < 16) |
| ScriptContext | `DataModel + 0x440` | Direct child pointer |
| ScriptContextIdentityLevel | `SC + 0x2C0` | int: identity tier (1-10) |
| ScriptContextActiveScript | `SC + 0x2B0` | Pointer to active script |
| RenderJobRealDataModel | `RenderJob + 0x1C8` | DataModel pointer in RenderJob |
| Job_Name | `job + 0x18` | MSVC SSO string: job name |
| JobStart / JobEnd | `TS + 0xC8 / 0xD0` | Pointer array of job objects |
