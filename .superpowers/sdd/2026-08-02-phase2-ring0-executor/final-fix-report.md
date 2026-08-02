# Phase 2 Final Review Fix Report

**Date:** 2026-08-03
**Branch:** main (worktree: phase2-ring0-executor)

## Fixed Findings

### C1 (CRITICAL): Cross-process allocation broken -- FIXED

**File:** `src/Injector/KernelExec.cpp`

**Root cause:** The kernel shellcode in `AllocateRemoteMemory` called `ZwAllocateVirtualMemory`
with pseudo-handle -1. Since the shellcode executes in the context of the calling thread
(the injector process), all allocations landed in the injector's address space, not
the target's. The `targetPid` parameter was accepted but silently discarded.

**Fix:** Completely rewrote both alloc and free shellcodes to use `KeStackAttachProcess`:

1. **Expanded `ShellcodeParams`** (0x98 bytes):
   - Added 4 new function pointers: `PsLookupProcessByProcessId`, `KeStackAttachProcess`,
     `KeUnstackDetachProcess`, `ObDereferenceObject`
   - Replaced `processHandle` with `targetPid`
   - Added `eprocess` output field and 0x30-byte `apcState` buffer (KAPC_STATE)

2. **New alloc shellcode** (0x92 bytes, 146 bytes):
   - Prologue allocates 0x38 bytes for shadow space + 2 stack args
   - Stores paramsBase at `[rsp+0x30]`; reloads via that slot after each call
     (r10 is volatile per x64 ABI) -- single paramsBase patch required
   - 5-step sequence: PsLookupProcessByProcessId -> KeStackAttachProcess ->
     ZwAllocateVirtualMemory(-1, ...) -> KeUnstackDetachProcess ->
     ObDereferenceObject
   - Error path: if PsLookupProcessByProcessId fails, stores NTSTATUS and returns

3. **New free shellcode** (0x85 bytes, 133 bytes):
   - Same 5-step pattern; simpler because ZwFreeVirtualMemory takes only
     4 register args
   - Prologue uses standard 0x28-byte shadow space

4. **Updated `AllocateRemoteMemory` / `FreeRemoteMemory`**:
   - Now resolve all 6 kernel function addresses at each call
   - Populate `targetPid` field with the caller's actual DWORD value
   - Compute jnz displacement at compile time via constexpr

5. **Static assertions** verify shellcode sizes at compile time.

### H1 (HIGH): PAGE section fallback risk -- FIXED

**File:** `src/Injector/KernelExec.cpp`, `Initialize()`

**Root cause:** When `.data` wasn't found, the section scan fell back to `PAGE`
sections. PAGE sections in ntoskrnl are pageable code (RX), not writable data.

**Fix:** Removed the `|| memcmp(section.Name, "PAGE", 4) == 0` condition.
Only `.data` is matched now. If `.data` is not found, `Initialize()` returns false.

Also increased the zero-fill scan chunk from 0x100 to 0x200 bytes to accommodate
the larger `ShellcodeParams` (0x98) + max shellcode (0x92) = 0x12A bytes.

### H2 (HIGH): ReadFromFile declared but never defined -- FIXED

**File:** `src/Injector/ManualMapInjector.h`

**Fix:** Removed the `ReadFromFile` declaration and its "File I/O" section header.
The function was never defined; file I/O is inlined in `Inject()`.

### H3 (HIGH): m_relocData declared but never used -- FIXED

**File:** `src/Injector/ManualMapInjector.h`

**Fix:** Removed the `m_relocData` member variable and its comment.
Relocation processing in `ApplyRelocations()` reads data directly from `m_rawDll`.

## Unchanged (not in scope for this fix)

- M1 (preferredBase ignored): Deferred
- M2 (Initialize return value unchecked): Deferred
- M3 (AdjustTokenPrivileges unverified): Deferred
- M4 (ControlService async race): Deferred
- M5 (no forwarded-export detection in kernel): Deferred
- M6 (linear O(n) kernel scan): Deferred
- L1 (stale shutdown comment): Deferred
- L2 (unused `<stdexcept>` includes): Deferred
- L3 (g_injectMode no validation): Deferred

## Build Verification

No build tools (CMake/MSBuild) were available in this environment.
Code review was performed manually with the following checks:

1. All x64 instruction encodings verified against Intel/AMD manuals
2. All struct offset calculations verified (ShellcodeParams layout matches
   shellcode byte patterns)
3. All jnz displacement calculations verified (0x5D for alloc, 0x50 for free)
4. Shellcode sizes match static_assert checks (0x92 alloc, 0x85 free)
5. Zero-fill scan buffer (0x200) exceeds total payload size (0x12A)
6. No residual references to `processHandle`, `ReadFromFile`, or `m_relocData`
   anywhere in src/
7. No remaining callers of removed declarations
