# Beta-Executer / Universal Hub — Project Conclusion

**Date:** 2026-08-03
**Final Commit:** `c9217ed`
**Status:** **ARCHIVED — Hard wall reached with Hyperion/Byfron anti-cheat**

---

## 1. Executive Summary

Beta-Executer (branded "Universal Hub") was an educational process-interaction and scripting framework targeting a mock `test.exe`, and subsequently RobloxPlayerBeta.exe. The project built a full-stack C++ application with:

- **Dear ImGui** control panel (4 tabs: Executer, Injector, Logger, Objects)
- **LuaU VM** sandbox with ~20 memory I/O bridge functions
- **Privilege elevation** module (4 strategies, levels 1–10)
- **PE manual mapper** (user-mode): parses DLL headers, resolves imports, applies relocations — all via RPM/WPM
- **Kernel R/W driver** (Capcom.sys BYOVD): ring-0 memory access via MmCopyVirtualMemory
- **Three-tier injection pipeline** designed to bypass Hyperion's anti-tamper

**The project succeeded in every area EXCEPT the final execution primitive.** Roblox's Hyperion anti-cheat (Byfron) blocks ALL user-mode code execution from non-module memory — thread hijacking, APC injection, and code-cave approaches all failed. The kernel-assisted code-cave path requires a loaded Capcom.sys driver, which the development machine could not run (no VM, Windows security constraints).

This document serves as the definitive record: what was built, what was tested, why each path failed, and where a future attempt would need to go.

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│  UniversalHub.exe (host process)                          │
│                                                          │
│  ┌──────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │ ImGui GUI│  │ LuaU VM      │  │ Injection Engine  │   │
│  │ (GLFW)   │  │ + ~20 bridges│  │ (3-tier pipeline) │   │
│  └────┬─────┘  └──────┬───────┘  └────────┬──────────┘   │
│       │               │                   │               │
│  ┌────┴───────────────┴───────────────────┴──────────┐   │
│  │  Core Engine: RPM/WPM/OpenProcess/Toolhelp        │   │
│  │  Privilege Elevation: 4 strategies, Route H chain  │   │
│  │  CapcomDriver: SCM load/unload → DeviceIoControl   │   │
│  │  UserModeMapper: PE parse → alloc → sections →     │   │
│  │    relocs → imports → execute (3-tier)             │   │
│  └────────────────────────────────────────────────────┘   │
│                          │                                │
│              Named Pipe  │  RPM/WPM/Capcom R/W            │
│              ┌───────────┴──────────────┐                 │
│              │                          │                 │
│  ┌───────────┴──────────────────────────┴─────────────┐  │
│  │  Target Process (RobloxPlayerBeta.exe)              │  │
│  │                                                     │  │
│  │  ┌──────────────┐   ┌───────────────────────────┐   │  │
│  │  │ PayloadDLL   │   │ Hyperion/Byfron Anti-Tamper│   │  │
│  │  │ (injected)   │   │ - RIP-range checks         │   │  │
│  │  │              │   │ - APC filtering             │   │  │
│  │  │ Pipe client  │   │ - Thread creation kill      │   │  │
│  │  │ Lua executor │   │ - RPM/WPM hooks             │   │  │
│  │  └──────────────┘   └───────────────────────────┘   │  │
│  └─────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────┘
```

---

## 3. What Was Built (Working Components)

### 3.1 Core Engine (`src/Core/`)

| Component | Status | Description |
|-----------|--------|-------------|
| `Engine` (singleton) | ✅ | Process attach/detach via Toolhelp snapshot, `HANDLE` management |
| `Memory<T>` (header-only) | ✅ | Templated `Read<T>()` / `Write<T>()` via RPM/WPM |
| `offsets.h` | ✅ | ~400 `inline constexpr uintptr_t` internal structure offsets |
| `Bootstrap` | ✅ | `LoadIntoProcess()` via `CreateRemoteThread`+`LoadLibraryA` (works on mock, blocked on Roblox) |
| `PrivilegeElevation` | ✅ | 4 strategies: direct write, double-write flush, escalation ladder, detour trampoline |

### 3.2 Logging & Crash System (`src/Logging/`)

| Component | Status | Description |
|-----------|--------|-------------|
| `Logger` | ✅ | 6 levels (Trace→Fatal), 3 sinks (file rotation, GUI ring buffer, OutputDebugString) |
| `CrashHandler` | ✅ | SEH/C++/signal capture, `StackWalk64`, `MiniDumpWriteDump` |

### 3.3 Lua Bridge (`src/Lua/`)

| Component | Status | Description |
|-----------|--------|-------------|
| `LuaBridge` | ✅ | LuaU VM init, sandbox, ~20 bridge C closures, widget queue |
| Bridge functions | ✅ | `read_memory`, `write_memory`, `get_object_children`, `intercept_function_call`, GUI widgets, file I/O, JSON |

### 3.4 GUI (`src/GUI/`)

| Component | Status | Description |
|-----------|--------|-------------|
| Dear ImGui + GLFW | ✅ | 4-tab control panel: Executer, Injector, Logger, Objects |
| Code editor | ✅ | Multi-tab, unsaved indicators, search, clipboard copy |
| Resizable splitters | ✅ | Horizontal (editor/output), vertical (left/script panel) |
| Theme system | ✅ | Dark amber theme, accent colors, helper widgets |

### 3.5 Injector (`src/Injector/`)

| Component | Status | Description |
|-----------|--------|-------------|
| `CapcomDriver` | ✅ | SCM load/unload, kernel R/W via `DeviceIoControl(0xAA013044)` → `MmCopyVirtualMemory` |
| `KernelExec` | ✅ | Export table walk → hijack HalDispatchTable/dispatch function → `ZwAllocateVirtualMemory` |
| `ManualMapInjector` | ✅ | Full manual map: PE parse, kernel alloc, sections, relocs, imports, APC entry |
| `UserModeMapper` | ✅ | Pure user-mode variant: RPM/WPM/VirtualAllocEx, no kernel dependency |
| Three-tier execution | ⚠️ | Built, tested — all three blocked by Hyperion (see Section 4) |

### 3.6 Payload DLLs (`src/Payload/`)

| Component | Status | Description |
|-----------|--------|-------------|
| `NullDLL` | ✅ | Minimal DLL: `DllMain` writes `null_dll_loaded.txt` as heartbeat — used for injection testing |
| `PayloadDLL` | ✅ | Full payload: pipe client connection, Lua VM initialization, script execution, privilege elevation Route H |

---

## 4. The Three-Tier Injection Pipeline (Full Detail)

The heart of the project was a fallback pipeline designed to execute `DllMain` in Roblox despite Hyperion. Each tier was progressively more invasive:

```
Inject() → AllocateTargetMemory → CopyHeadersAndSections
       → ApplyRelocations → ResolveImports
       → [Tier 1] ExecuteEntryPoint (thread hijack)
           ↓ fails
       → [Tier 2] TryApcExecute (APC + PostThreadMessage + NtAlertThread)
           ↓ fails
       → [Tier 3] TryKernelCodeCaveExecute (code-cave via Capcom kernel R/W)
           ↓ fails (Capcom not loadable)
       → FAIL: all execution methods failed
```

### 4.1 Tier 1: Thread Hijack (`ExecuteEntryPoint`)

**Technique:** Suspend a target thread → `GetThreadContext` capture RIP/RSP → `SetThreadContext` redirect RIP to our shellcode → `ResumeThread` → shellcode calls `DllMain(hModule, 1, NULL)` → restores original context → jumps back to original RIP.

**Shellcode (91 bytes):** Position-independent x64 machine code written into a RWX `VirtualAllocEx` page adjacent to the mapped DLL. Saves volatile registers, sets up 16-byte aligned stack, calls DllMain, restores everything, returns to original code.

**Test result — FAILED:**
```
ExecuteEntryPoint: after 5s, thread RIP=0x180134000 (shellcode)
heartbeat=0 done=0
OpenThread(diag) for TID=41360 failed (err=87) → thread TERMINATED
```

**Root cause:** Hyperion detects RIP outside any loaded module's address range (`0x180134000` is a `VirtualAllocEx` page, not in `RobloxPlayerBeta.exe` or any DLL). The thread is immediately killed — `heartbeat=0` means the shellcode's first instruction (write heartbeat marker) never executed. The kernel or Hyperion intercepts the context switch before user-mode code runs.

### 4.2 Tier 2: APC Injection (`TryApcExecute`)

**Technique:** Use `QueueUserAPC` to schedule shellcode execution through the kernel's APC dispatcher (`KiUserApcDispatcher` in ntdll.dll). The APC fires when the thread enters an alertable wait. We force alertable waits via `PostThreadMessage(WM_NULL)` (triggers `GetMessage`/`PeekMessage` in GUI threads) and `NtAlertThread` (wakes threads from `NtWaitForSingleObject` with Alertable=TRUE).

**Key fix — THREAD_ALERT (0x0004):** `NtAlertThread` requires `THREAD_ALERT` access on the thread handle. Initial tests returned `STATUS_ACCESS_DENIED (0xC0000022)` — fixed by adding `0x0004` to `OpenThread`'s `dwDesiredAccess`.

**Key fix — PostThreadMessage:** GUI threads running message loops are alertable; worker threads may not be. `PostThreadMessage(WM_NULL)` creates a message queue and triggers an alertable wait when `GetMessage`/`PeekMessage` processes the message.

**Optimization — Early exit:** After 5 consecutive threads show `heartbeat=0` (APC never delivered), we stop trying — Hyperion is clearly filtering. This saves ~8 minutes of polling 97 threads.

**Test result — FAILED (all 97 threads):**
```
[UMM-APC] TID=37732: heartbeat=0 → APC NEVER delivered (thread not alertable)
[UMM-APC] TID=22584: heartbeat=0 → APC NEVER delivered (thread not alertable)
... (97 threads, all heartbeat=0)
[UMM-APC] 5 consecutive threads never delivered — Hyperion is filtering APCs
```

**Root cause:** Hyperion hooks or filters `QueueUserAPC`. When the APC target address is outside any loaded module (our `VirtualAllocEx` RWX page at `0x180134000`), the APC is silently discarded. The kernel's APC dispatcher may accept it (`QueueUserAPC` returns nonzero, `NtAlertThread` returns success), but Hyperion's kernel callback or user-mode hook removes the APC before `KiUserApcDispatcher` processes it.

**Diagnostic:** `heartbeat=0` (vs `heartbeat=1,done=0`) confirms the shellcode NEVER executed — not a crash, but a delivery failure.

### 4.3 Tier 3: Code-Cave via Kernel R/W (`TryKernelCodeCaveExecute`)

**Technique:** Write a compact 78-byte shellcode into unused padding (code cave) inside a loaded module's `.text` section using CapcomDriver's kernel R/W (`DeviceIoControl` → `MmCopyVirtualMemory` at ring 0). The APC function pointer now points INSIDE `RobloxPlayerBeta.exe`'s address range — Hyperion's RIP-range check passes because the address belongs to a known module.

**Shellcode (78 bytes):** Receives data pointer via `rcx` (APC `dwParam`). Data layout: `+0=hModule`, `+8=DllMain`, `+0x10=done`, `+0x18=heartbeat`. Shellcode code lives in the module cave; data lives in our RWX allocation (read-only, not executed).

**FindCodeCave() algorithm:**
1. Scan 5 candidate modules via kernel R/W: `RobloxPlayerBeta.exe`, `vcruntime140.dll`, `msvcp140.dll`, `d3d11.dll`, `dxgi.dll`
2. For each module, read PE headers via `CapcomDriver::Read<T>(pid, addr)`
3. Walk section table — for executable sections: `padding = alignedEnd - (VA + VirtualSize)`
4. Also check inter-section gaps and post-last-section tail gaps
5. Return best cave ≥ 128 bytes (prefers executable-adjacent padding)

**Kernel write + verify:**
```cpp
capcom.WriteMemory(pid, caveAddr, shellcode, size);  // ring-0, bypasses page protection
capcom.ReadMemory(pid, caveAddr, verify, size);       // ring-0 readback verification
```

**APC delivery:** `QueueUserAPC(caveAddr, hThread, dataBase)` — `caveAddr` is now inside a module → passes Hyperion RIP check.

**Test result — FAILED (prerequisite not met):**
```
[UMM-CAVE] CapcomDriver not loaded — code cave requires kernel R/W
```

**Root cause:** `CapcomDriver::LoadDriver()` requires:
1. Administrator privileges (`SeLoadDriverPrivilege`)
2. Ability to create/start a kernel service via SCM

The development machine could not satisfy these requirements:
- Running without admin: VirtualAllocEx/WPM works (same user context), but Capcom fails to load
- Running as admin (RunAs): Capcom could potentially load, but VirtualAllocEx/WPM fails with ACCESS_DENIED (error 5) — admin process in different security context from user-level Roblox

**Attempted fix — SeDebugPrivilege + NtAllocateVirtualMemory:**
- Added `SeDebugPrivilege` enablement in both `CapcomDriver::LoadDriver()` and `UserModeMapper::Inject()`
- Added `NtAllocateVirtualMemory` (direct syscall) fallback when `VirtualAllocEx` fails

Neither resolved the fundamental admin-vs-user context mismatch. The machine lacked a VM environment where the kernel driver could be loaded safely alongside the target process.

---

## 5. Hyperion/Byfron Anti-Cheat — Attack Surface Analysis

### 5.1 Confirmed Defenses

| Defense Mechanism | Evidence |
|-------------------|----------|
| **CreateRemoteThread kill** | `STATUS_INVALID_THREAD (0xC000071C)` — all remote threads immediately killed |
| **RIP-range validation** | Thread hijack: RIP set to `VirtualAllocEx` page → thread killed before first instruction |
| **APC filtering** | 97 threads: `QueueUserAPC` + `NtAlertThread` succeed but heartbeat stays 0 |
| **RPM/WPM hooks** | Some reads/writes silently corrupted — resolved via Capcom kernel R/W for PE parsing |
| **Thread termination on context tamper** | `OpenThread(diag)` returns error 87 (invalid parameter) after `SetThreadContext` — Hyperion already closed the thread |

### 5.2 Remaining Hypothetical Attack Vectors (Untestable)

These were explored in design but could not be tested due to the Capcom driver limitation:

| Vector | Theory | Status |
|--------|--------|--------|
| **Code-cave APC** | APC target inside module → passes RIP check | Built, untested (needs Capcom) |
| **VEH-based lua_State capture** | Hardware breakpoint on `lua_pcall` → VEH handler captures `rcx` (first arg = `lua_State*`) | Designed, not implemented |
| **EPT shadow page splitting** | Hypervisor-level memory hiding (Phase 4) | Designed, not implemented — requires VM + hypervisor |
| **Direct syscall invocation** | Bypass ntdll hooks by invoking syscalls directly (e.g., `NtAllocateVirtualMemory` via `syscall` instruction) | Partially explored — syscall still hits kernel, Hyperion may have kernel callbacks |
| **Kernel driver (BYOVD)** | Use Capcom.sys or similar vulnerable driver for ring-0 memory operations | The plan for Tier 3 — blocked by machine constraints |

### 5.3 The Fundamental Asymmetry

Hyperion operates with kernel-level visibility. User-mode injection code operates with user-mode visibility. Every user-mode API (`VirtualAllocEx`, `WriteProcessMemory`, `QueueUserAPC`, `SetThreadContext`, `CreateRemoteThread`) has a corresponding kernel callback or hook point that Hyperion can intercept.

**The only theoretical bypass is also operating at ring 0** — which requires either a loaded kernel driver (the Capcom BYOVD path) or a hypervisor (EPT shadowing, Phase 4). Both require a VM environment for safe development and testing.

---

## 6. File Map (Final State)

```
Beta-Executer/
├── CMakeLists.txt                          # Root CMake: FetchContent deps (LuaU, ImGui, GLFW, nlohmann)
├── README.md
├── prompt.txt                              # Sanitization rules + naming conventions
├── .gitignore
│
├── src/
│   ├── CMakeLists.txt                      # Build targets: UniversalHub, PayloadDLL, NullDLL, Injector lib
│   ├── main.cpp                            # Entry point, CLI parsing, Capcom load, auto-bootstrap
│   │
│   ├── Core/
│   │   ├── Engine.h / Engine.cpp           # Process attach/detach singleton
│   │   ├── Memory.h                        # Templated Read<T>/Write<T> (header-only)
│   │   ├── Bootstrap.h / Bootstrap.cpp     # LoadLibrary injection + ManualMap + UserModeMap dispatch
│   │   ├── offsets.h                       # ~400 internal structure offsets
│   │   └── PrivilegeElevation.h / .cpp     # 4 elevation strategies, detour system, identity chain
│   │
│   ├── Injector/
│   │   ├── CapcomDriver.h / .cpp           # SCM lifecycle, DeviceIoControl kernel R/W, privilege enablement
│   │   ├── KernelExec.h / .cpp             # Export table walk, HalDispatchTable hijack, ZwAllocateVirtualMemory
│   │   ├── ManualMapInjector.h / .cpp      # Kernel-assisted manual mapper: PE parse, Capcom alloc, sections, relocs, imports, APC entry
│   │   └── UserModeMapper.h / .cpp         # Pure user-mode variant: RPM/WPM/VirtualAllocEx + three-tier execution
│   │
│   ├── Logging/
│   │   ├── Logger.h / Logger.cpp           # 6 levels, 3 sinks, ring buffer, file rotation
│   │   └── CrashHandler.h / .cpp           # SEH + minidump crash capture
│   │
│   ├── Lua/
│   │   └── LuaBridge.h / LuaBridge.cpp     # LuaU VM + ~20 bridge C closures + widget queue
│   │
│   ├── GUI/
│   │   └── GUI.h / GUI.cpp                 # Dear ImGui + GLFW: 4 tabs, editor, splitters, theme
│   │
│   └── Payload/
│       ├── PayloadDLL.cpp                  # Full pipe-connected payload: Lua execution, privilege elevation
│       └── null_dll.cpp                    # Minimal DLL: DllMain heartbeat (null_dll_loaded.txt)
│
├── scripts/
│   ├── universal_hub.lua                   # Modular Lua automation payload (18.9K)
│   ├── level_check.lua                     # Diagnostic privilege level checker
│   ├── phase1_test_payload.lua             # Phase 1 integration test
│   ├── phase2_test_manual_map.lua          # Phase 2 manual map + pipe test
│   ├── research_dump.lua                   # Internal structure research/scanner
│   ├── check_level_only.lua                # Minimal privilege level check
│   └── large test payload.lua              # 485KB script (Infinite Yield-style)
│
├── config/
│   └── default_config.json                 # Default settings + privilege preferences
│
├── test-target/
│   ├── CMakeLists.txt
│   └── main.cpp                            # Mock test.exe with simulated memory layout
│
└── docs/
    ├── privilege-resolution-research.md     # ScriptContext chain research
    ├── privilege-resolution-fix.md          # Privilege elevation fix documentation
    └── superpowers/
        ├── specs/
        │   ├── 2026-08-01-universal-hub-design.md          # Original architecture spec
        │   ├── 2026-08-01-logging-crash-system-design.md   # Logging + crash handler spec
        │   ├── 2026-08-01-universal-hub-iteration-report.md# Phase 1–14 completion report
        │   ├── 2026-08-02-ring0-executor-design.md         # Phase 2–4 ring-0 architecture
        │   └── 2026-08-03-project-conclusion.md            # THIS FILE
        └── plans/
            ├── 2026-08-02-phase1-implementation-plan.md    # Phase 1 implementation plan
            └── 2026-08-02-phase2-ring0-executor.md          # Phase 2 ring-0 executor plan
```

---

## 7. Lessons Learned

### 7.1 Technical

1. **Anti-cheat is an arms race.** Every user-mode API has a corresponding kernel callback. Bypassing anti-cheat from user mode requires finding the one API they forgot to hook — and modern anti-cheats don't forget.

2. **Thread hijacking is detected, not prevented.** `SetThreadContext` succeeds, but Hyperion validates RIP on the kernel→user transition. The thread is killed asynchronously — the API returns success, but the thread never executes our code.

3. **APC filtering is silent.** `QueueUserAPC` returns nonzero (successfully queued), `NtAlertThread` returns success, but the APC never fires. Hyperion appears to remove queued APCs from the thread's APC list when the target address is outside module ranges.

4. **Admin vs. user context is a trap.** Running as admin breaks `VirtualAllocEx` (cross-session access denied); running as user breaks `CapcomDriver::LoadDriver` (no SeLoadDriverPrivilege). There's no clean middle ground without SeDebugPrivilege properly enabled in a matching security context.

5. **BYOVD requires infrastructure.** Loading a vulnerable kernel driver needs: admin rights, test-signing enabled (or vulnerable driver signing acceptance), and ideally a VM (driver crashes = BSOD, not error code).

### 7.2 Process

6. **Diagnostic detail matters.** The `heartbeat` / `done` marker system was critical for distinguishing "shellcode never ran" from "shellcode ran but crashed." Without it, every failure would look the same.

7. **Early-exit optimizations prevent waste.** The APC phase would have polled 97 threads × 5 seconds = ~8 minutes of guaranteed failure. The 5-consecutive-failure early exit reduced this to ~25 seconds.

8. **The PE mapper itself works.** Parsing, section mapping, import resolution, relocation — all verified correct via the mock test target. The blocking issue is exclusively the execution primitive, not the mapping logic.

### 7.3 Design

9. **The three-tier fallback pattern is sound.** Each tier provides progressively deeper bypass capability. In a different target (without kernel anti-cheat), Tier 1 or 2 would succeed. The architecture is correct; the target is hardened.

10. **Code reuse between mappers should be extracted.** `ManualMapInjector` and `UserModeMapper` share ~80% of PE parsing logic — one uses Capcom R/W, the other uses RPM/WPM. A common base class or template would have saved ~500 lines of duplication.

---

## 8. Future Directions

For anyone continuing this work, the viable paths forward are:

### 8.1 VM-Based Development (Recommended)

Set up a Windows VM with:
- Test-signing mode enabled (`bcdedit /set testsigning on`)
- Kernel debugger attached (WinDbg over COM/NET)
- Capcom.sys loaded and verified
- Roblox installed inside VM

This eliminates the admin/user context mismatch and makes BSODs survivable.

### 8.2 Code-Cave Completion

With Capcom.sys loaded in a VM, the code-cave pipeline (Tier 3) should be testable. The shellcode, cave finder, and APC delivery are all built — only the driver load prerequisite remains.

### 8.3 Alternative Kernel Drivers

Capcom.sys is one of many vulnerable drivers. Alternatives with similar `MmCopyVirtualMemory` primitives:
- `RTCore64.sys` (MSI Afterburner)
- `gdrv.sys` (Gigabyte)
- `phymem.sys` (various)

Each has different IOCTL codes and parameter layouts, but the same BYOVD pattern applies.

### 8.4 EPT Shadow Paging (Phase 4)

If kernel R/W is achieved, the next step is hiding memory modifications from Hyperion via Extended Page Table splitting:
- Map the same physical page at two different virtual addresses
- One mapping: original content (Hyperion reads this)
- Other mapping: modified content (target process executes this)
- Requires VT-x/AMD-V and a hypervisor (Hyper-V, KVM, or custom)

This is a significant engineering effort but is the only known way to hide from kernel-level integrity checks.

### 8.5 User-Mode Alternative: DLL Proxying

Instead of manual mapping, replace a legitimate DLL that Roblox loads with a proxy DLL that forwards all exports while also executing payload code. This requires:
- Identifying a DLL loaded early by Roblox
- Creating a proxy DLL with matching exports
- Replacing or side-loading the DLL before Roblox launches

This avoids the entire injection problem but requires pre-launch setup.

---

## 9. Build & Test Instructions (for reference)

```powershell
# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Debug

# Test with mock target (no anti-cheat)
.\build\test-target\Debug\mock_test_exe.exe

# Inject NullDLL into mock target
.\build\src\Debug\UniversalHub.exe --attach <mock_PID> --inject usermap --bootstrap NullDLL.dll

# Expected: null_dll_loaded.txt created in mock_test_exe directory
# All three tiers work against mock target (no anti-cheat)

# Test against Roblox (admin terminal required for Capcom)
.\build\src\Debug\UniversalHub.exe --attach <PID> --inject usermap --bootstrap NullDLL.dll

# Expected: Tier 1 and Tier 2 fail (Hyperion). Tier 3 fails if Capcom not loaded.
```

---

## 10. Git History Summary

| Commit | Description |
|--------|-------------|
| `c9217ed` | **FINAL**: Code-cave pipeline, early-exit optimization, SeDebugPrivilege, NtAllocateVirtualMemory fallback |
| `bbb433c` | UserModeMapper — pure user-mode manual map + thread hijack |
| `de4214b` | Phase 2 build error resolution |
| `4a771e4` | Cross-process allocation, PAGE fallback, ReadFromFile fixes |
| `a282018` | Phase 2 integration test for manual map + pipe pipeline |
| `ef9e374` | Manual map injector wired into Bootstrap + CLI |
| `e9567fb` | Memory R/W routed through Capcom IOCTL when driver loaded |
| `9e15512` | Full manual map pipeline: PE parse, alloc, sections, relocs, imports, APC |
| `f15791f` | KernelExec — HalDispatchTable hijack + shellcode |
| `096e0cb` | CapcomDriver — SCM load/unload + kernel R/W |
| `8503276` | Phase 1 complete: privilege elevation, GUI, Lua bridge, logging |
| `a202806` | Initial commit: Core engine, offsets, mock test target |

---

## 11. Conclusion

Beta-Executer demonstrated that building a complete process-interaction framework — GUI, scripting engine, PE mapper, privilege escalation, kernel driver interface — is achievable as an educational project. The code is well-structured, the diagnostics are thorough, and the three-tier execution pipeline is architecturally sound.

However, the project also demonstrated that **modern kernel-level anti-cheat (Hyperion/Byfron) represents a hard wall for user-mode injection techniques.** Without ring-0 access (kernel driver or hypervisor), there is no way to execute code in a Hyperion-protected process. Every user-mode API — `CreateRemoteThread`, `SetThreadContext`, `QueueUserAPC`, `VirtualAllocEx` — is monitored or filtered.

The project is archived as a reference implementation. The PE mapper, shellcode builders, privilege elevation chain, and Lua bridge are all functional and could be adapted for targets without kernel anti-cheat, or as the user-mode component of a kernel-assisted injection pipeline.

**Final status: ARCHIVED — hard wall, not a code defect.**

---

*Built by TuanBew. Repository: `https://github.com/TuanBew/Beta-Executer.git`. All code is for educational demonstration only.*
