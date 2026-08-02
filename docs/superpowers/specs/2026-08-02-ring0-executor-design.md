# Ring-0 Executor Architecture — Design Specification

**Date:** 2026-08-02
**Status:** Design approved, awaiting implementation plan
**Scope:** Multi-layer script execution system with kernel privilege

---

## Overview

Universal Hub currently executes Lua scripts in a host-side Luau VM with external memory I/O bridge functions (RPM/WPM). This architecture cannot run scripts that require access to the target process's internal Lua environment (global state, internal services, instance tree API, etc.) which only exists inside the target process.

This specification defines a four-phase architecture that:

1. Injects a DLL into the target via kernel-assisted manual mapping (bypassing user-mode AC hooks)
2. Captures the target's `lua_State*` non-invasively (hardware breakpoint + VEH)
3. Elevates script identity to level 10 (reusing existing Route H + privilege code)
4. Executes arbitrary Lua payloads inside the target's Lua VM via a persistent Lua-level Script Manager
5. (Phase 4) Hides all modifications from Hyperion via EPT shadow page splitting

---

## Phase 1: User-Mode (Ring 3) Prototyping & Logic Validation

### 1.1 Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│  UniversalHub.exe (Ring 3)                                           │
│                                                                      │
│  ┌────────────┐   ┌─────────────────┐   ┌────────────────────────┐   │
│  │  GUI/CLI   │   │ Manual Map       │   │ Named Pipe Server      │   │
│  │  (ImGui)   │   │ Injector         │   │ \\.\pipe\UniversalHub  │   │
│  └─────┬──────┘   │ (Phase 2)        │   └───────────┬────────────┘   │
│        │          └────────┬────────┘               │                │
│        │                   │                        │                │
├────────┼───────────────────┼────────────────────────┼────────────────┤
│        │                   │   test.exe                             │                │
│        │                   │   ┌────────────────────┼─────────────┐  │
│        │                   │   │  Injector DLL      │             │  │
│        │                   │   │                    │             │  │
│        │                   │   │  1. Init pipe client◄────────────┘  │
│        │                   │   │  2. VEH on lua_pcall               │  │
│        │                   │   │  3. → captures lua_State*          │  │
│        │                   │   │  4. Route H → ScriptContext        │  │
│        │                   │   │  5. Write level 10 to SC+0x2C0     │  │
│        │                   │   │  6. Heartbeat hijack (ONE-SHOT)    │  │
│        │                   │   │  7. → inject Lua Script Manager    │  │
│        │                   │   │  8. Restore original Heartbeat     │  │
│        │                   │   │                    │               │  │
│        │                   │   │  ┌─ Script Manager (Lua) ───────┐  │  │
│        │                   │   │  │ • Heartbeat callback poll    │  │  │
│        │                   │   │  │ • Named pipe command reader  │  │  │
│        │                   │   │  │ • loadstring() executor      │  │  │
│        │                   │   │  │ • Status reporter            │  │  │
│        │                   │   │  │ • Coroutine wrapper (hot-swap)│  │  │
│        │                   │   │  └──────────────────────────────┘  │  │
│        │                   │   │                    │               │  │
│        │                   │   │  ┌─ Target Lua VM ──────────────┐  │  │
│        │                   │   │  │ global state, internal APIs, │  │  │
│        │                   │   │  │ instance tree, core services │  │  │
│        │                   │   │  └──────────────────────────────┘  │  │
│        │                   │   └────────────────────────────────────┘  │
└────────┴───────────────────┴──────────────────────────────────────────┘
```

### 1.2 Step-by-Step Data Flow

#### 1.2.1 Named Pipe Protocol

Binary framed protocol over Windows named pipe (`\\.\pipe\UniversalHub`) in message mode (`PIPE_TYPE_MESSAGE`).

```
┌──────────┬─────────┬──────────┬────────────┬──────────────────────┐
│ Magic    │ Version │ Command  │ PayloadLen │ Payload              │
│ 4 bytes  │ 1 byte  │ 2 bytes  │ 4 bytes LE │ PayloadLen bytes     │
│ "HUB!"   │ 0x01    │          │            │                      │
│ 0x48554221│        │          │            │                      │
└──────────┴─────────┴──────────┴────────────┴──────────────────────┘
```

**Commands:**

| ID   | Name            | Direction     | Payload                                    |
|------|-----------------|---------------|--------------------------------------------|
| 0x01 | EXECUTE_SCRIPT  | Hub → DLL     | Raw Lua source code (up to ~2GB)           |
| 0x02 | EXECUTE_RESULT  | DLL → Hub     | 1B status (0=ok, 1=error, 2=fatal) + null-terminated UTF-8 error string if error |
| 0x03 | PING            | Hub → DLL     | Empty                                      |
| 0x04 | PONG            | DLL → Hub     | 4B LE: current script state flags           |
| 0x05 | SHUTDOWN        | Hub → DLL     | Empty (kills Script Manager, detaches VEH) |
| 0x06 | SCRIPT_CHUNK    | Hub → DLL     | (Future) 2B chunk_index + 2B total_chunks + chunk data — for scripts >1MB |

**Future note:** Commands 0x06+ reserved for chunked transfer of very large scripts.

#### 1.2.2 lua_State* Capture (Hardware Breakpoint + VEH)

1. DLL resolves `lua_pcall` address: `GetProcAddress(GetModuleHandleA("Luau"), "lua_pcall")`
2. Sets hardware breakpoint on `lua_pcall` via `SetThreadContext` on the main target thread:
   - `DR0` = address of `lua_pcall`
   - `DR7` = enable local breakpoint on DR0
3. Installs `AddVectoredExceptionHandler(1, handler)` — priority 1 VEH
4. On `EXCEPTION_SINGLE_STEP`:
   - Check `ExceptionAddress == lua_pcall`
   - Read `lua_State*` from RCX (x64 calling convention, first argument)
   - Store `lua_State*` globally
   - Disable DR0/DR7
   - Resume execution (`EXCEPTION_CONTINUE_EXECUTION`)

Hardware breakpoints do not modify code bytes, making them invisible to CRC-based integrity checks.

**Fallback (if hardware breakpoints are monitored):** Write a `jmp` trampoline at `lua_pcall` entry, capture `lua_State*`, restore original bytes, call original `lua_pcall`.

#### 1.2.3 Privilege Elevation (Phase 1.5)

**CRITICAL:** Must execute BEFORE the Script Manager injection. The captured `lua_State*` may belong to a low-privilege script. We must elevate identity to level 10 first so the Script Manager (and all scripts it executes) run at maximum privilege.

1. **Resolve ScriptContext** — reuse existing Route H heap scan (`PrivilegeElevation::ScanForScriptContext`). Already proven working: finds ScriptContext in ~270ms in the live target.
2. **Write identity level** — `Memory::Write<int>(scriptContext + 0x2C0, 10)` (via Capcom IOCTL in Phase 2, or direct WPM in Phase 1 dev)
3. **Set active script pointer** — `Memory::Write<uintptr_t>(scriptContext + 0x2B0, ourScriptObject)` — points the ScriptContext to our script
4. **Verify** — read back identity level, confirm it's 10

This reuses the entire existing privilege elevation subsystem from `src/Core/PrivilegeElevation.cpp`.

#### 1.2.4 Heartbeat Hijack (One-Shot)

The Heartbeat trampoline fires **once** to inject the Lua Script Manager, then restores original bytes immediately.

1. Locate Heartbeat job in TaskScheduler array (walk jobs, find name containing "Heartbeat") — existing code already enumerates jobs with readable names
2. Read function pointer from job object (determine exact offset by dumping job — candidate offsets: +0x10 or +0x28)
3. Write a `jmp` (5 bytes, `E9 <rel32>`) at the Heartbeat function entry
4. In the trampoline handler:
   a. Call `luau_load` + `lua_pcall` with the Lua Script Manager source (stored in DLL's `.rdata`)
   b. Restore original 5 bytes at Heartbeat function entry
   c. `jmp` back to original Heartbeat function (tail call — no stack frame pollution)
5. The trampoline exists for **one frame only**. After restoration, no C++ hooks remain.

**Why one-shot:** After the first frame, there are no C++ hooks, no trampolines, and no modified code in the target's modules. Hyperion's periodic integrity checks find nothing.

#### 1.2.5 Lua Script Manager (Persistent, Lua-Level)

The injected Script Manager runs as legitimate Lua code inside the target's scheduler. It uses the target's own heartbeat scheduling API — indistinguishable from any normal script.

```lua
-- Injected Script Manager (embedded in DLL's .rdata section)
local PIPE_NAME = "\\\\.\\pipe\\UniversalHub"
local pipe = ... -- opened from C++ side, passed via global

-- Register with the target's heartbeat scheduler
get_scheduler().Heartbeat:Connect(function()
    -- Poll pipe for new commands (non-blocking)
    local cmd, payload = readFrame(pipe)
    if cmd == 0x01 then  -- EXECUTE_SCRIPT
        local ok, err = pcall(function()
            local fn = loadstring(payload)
            fn()
        end)
        writeResult(pipe, ok and 0 or 1, err)
    elseif cmd == 0x05 then  -- SHUTDOWN
        -- Clean up and disconnect
    end
end)
```

**Capabilities:**
- Polling pipe on every Heartbeat tick (zero C++ hooks)
- `loadstring()` execution for arbitrary payloads
- Coroutine wrapper for hot-swap (kill old script, start new one)
- Status reporting back to UniversalHub (success/error/fatal)

---

## Phase 2: Kernel Driver (Ring 0) Foundation

### 2.1 Capcom.sys BYOVD

**Driver:** Capcom.sys
**IOCTL:** `0xAA013044`
**Capability:** Arbitrary kernel-mode read/write via `MmCopyVirtualMemory`

The Capcom driver provides `CTL_CODE(FILE_DEVICE_UNKNOWN, 0x0800, METHOD_BUFFERED, FILE_ANY_ACCESS)` which calls `MmCopyVirtualMemory` with user-supplied source/destination process IDs.

Both `ReadProcessMemory` and `WriteProcessMemory` calls from UniversalHub are rewritten to go through Capcom IOCTLs, bypassing any user-mode hooks Hyperion may have installed.

### 2.2 Kernel Function Execution Primitive

**Problem:** Capcom provides R/W but not `ZwAllocateVirtualMemory` for remote processes. The manual map injector needs to allocate memory in the target.

**Solution:** Build a kernel function execution primitive on top of Capcom's R/W:

1. Allocate a small shellcode buffer in kernel memory (use Capcom to write to a known kernel pool address obtained via `NtQuerySystemInformation` → `SystemModuleInformation`)
2. Write shellcode that calls `ZwAllocateVirtualMemory` with the target process handle
3. Find a rarely-called kernel function pointer (via `SystemModuleInformation` export table walk)
4. Overwrite function pointer with shellcode address (using Capcom write)
5. Trigger execution by calling the hijacked function from user mode
6. Restore original function pointer after allocation completes

This is a well-documented pattern in the BYOVD community.

### 2.3 Manual Map Injector

A C++ class (`ManualMapInjector`) in UniversalHub that operates entirely through Capcom IOCTLs:

1. **Parse PE headers** — read DLL file, extract `IMAGE_OPTIONAL_HEADER`, sections, import directory, relocations
2. **Allocate memory** — via kernel function execution primitive → `ZwAllocateVirtualMemory(targetProcess, &baseAddr, 0, &sizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)`
3. **Write PE headers and sections** — copy headers + each section into the allocated block using Capcom writes
4. **Apply relocations** — calculate delta (actual - preferred base), fix up all `.reloc` entries
5. **Resolve imports** — walk target's PEB → Ldr → module list via Capcom reads; for each imported DLL, parse its export table and resolve function addresses; write into IAT
6. **Set correct page protection** — change sections to correct protections (.text → PAGE_EXECUTE_READ, .rdata → PAGE_READONLY, etc.)
7. **Execute entry point** — write a small shellcode stub that calls `DllMain(hModule, DLL_PROCESS_ATTACH, NULL)`, queue it via APC to an existing thread, then free shellcode region after return
8. **The DLL is invisible** — not in module list (`EnumProcessModules`), no `LoadLibraryA` call, no `LdrLoadDll` trace

### 2.4 Integration With Phase 1

The Phase 1 DLL is the payload for the manual map injector. Once mapped:
- DLL's entry point sets up VEH handler and named pipe client
- All subsequent memory operations (privilege elevation, etc.) use the kernel driver path
- Entire toolchain operates at Ring 0 privilege level

### 2.5 Testing Milestones

1. Validate manual mapping with a "Hello World" DLL (creates a file to confirm entry point execution)
2. Replace test DLL with Phase 1 DLL, confirm pipe communication
3. Verify Lua execution against test.exe

---

## Phase 3: Integration Layer (Ring 3 ↔ Ring 0)

### 3.1 Component Wiring

```
UniversalHub.exe
    │
    ├─► CapcomHandle ──► DeviceIoControl(0xAA013044) ──► MmCopyVirtualMemory
    │
    ├─► ManualMapInjector ──► CapcomHandle ──► DLL injected into target
    │
    ├─► NamedPipeServer ──► \\.\pipe\UniversalHub ──► DLL's pipe client
    │
    └─► GUI displays: connection status, script state, error log
```

### 3.2 Startup Sequence

1. **Load Capcom.sys** — create/start service, obtain device handle
2. **Manual-map DLL** — inject Phase 1 DLL into target via `ManualMapInjector`
3. **Wait for pipe connection** — DLL connects back, sends initial PONG/STATUS with ready flag
4. **Trigger lua_State capture** — DLL's VEH fires on next `lua_pcall`, captures `lua_State*`
5. **Elevate privilege** — write level 10 to ScriptContext+0x2C0 (via Capcom)
6. **Heartbeat hijack** — one-shot trampoline injects Script Manager
7. **Ready state** — pipe reports "ready", UniversalHub enables Execute button
8. **Execute script** — user clicks Execute → Hub sends EXECUTE_SCRIPT frame → Script Manager `loadstring()`s it → result reports back

### 3.3 Shutdown Sequence

1. Send SHUTDOWN command via pipe
2. Script Manager kills any running user scripts (coroutine cleanup)
3. Script Manager disconnects pipe
4. UniversalHub closes pipe server
5. No DLL cleanup needed (lives as long as target process)

---

## Phase 4: Advanced System Features (Ring -1 / Hypervisor)

### 4.1 EPT Shadow Page Splitting

**Base:** HyperPlatform (tandasat) or DdiMon — Intel VT-x hypervisors with existing EPT hooking logic.

**Scope:** Virtualize only the target process's CR3. No full-system virtualization needed. EPTs are set per-VCPU and switched on VM-entry/exit based on the active process.

### 4.2 Dual EPT Views

```
┌─────────────────────────────────────────────────────────┐
│ Primary EPT (CPU execution)                              │
│ ┌────────────────────────────────────────────────────┐  │
│ │ Roblox .text  → modified pages (injected DLL,      │  │
│ │                  trampolines, hooks)                │  │
│ │ Roblox .data  → modified (elevated identity fields)│  │
│ └────────────────────────────────────────────────────┘  │
│                                                          │
│ Shadow EPT (activated during Hyperion scans)             │
│ ┌────────────────────────────────────────────────────┐  │
│ │ Roblox .text  → original, unmodified pages          │  │
│ │ Roblox .data  → original values (before elevation)  │  │
│ └────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

### 4.3 Trigger Mechanism

1. Mark modified pages as read-only in the Primary EPT
2. When an EPT violation occurs, check the faulting RIP
3. If RIP belongs to Hyperion's kernel driver (known address range), switch to Shadow EPT temporarily
4. Let the read complete → Hyperion sees clean memory
5. Switch back to Primary EPT
6. Normal execution continues with modified pages

### 4.4 Integration Note

**After Phase 4 is complete, Capcom.sys and the manual map injector become optional.** The hypervisor can directly handle memory hiding and code injection through EPT manipulation. However, the Capcom path remains useful for:
- Initial hypervisor installation (loading the hypervisor driver)
- Non-virtualized development/testing
- Fallback if VT-x is unavailable

---

## Protocol Reference

### Frame Structure (Little-Endian)

```
Offset  Size  Field       Description
─────────────────────────────────────────
0x00    4     Magic        0x48554221 ("HUB!")
0x04    1     Version      0x01
0x05    2     Command      See command table
0x07    4     PayloadLen   Length of Payload field
0x0B    N     Payload      Command-specific data
```

### Command Table

| Value | Name           | Dir     | Payload Format                              |
|-------|----------------|---------|---------------------------------------------|
| 0x01  | EXECUTE_SCRIPT | H→D     | Raw Lua source (UTF-8)                      |
| 0x02  | EXECUTE_RESULT | D→H     | 1B status + error_str (null-term UTF-8)     |
| 0x03  | PING           | H→D     | Empty                                       |
| 0x04  | PONG           | D→H     | 4B state_flags (LE uint32)                  |
| 0x05  | SHUTDOWN       | H→D     | Empty                                       |
| 0x06  | SCRIPT_CHUNK   | H→D     | (Future) 2B index + 2B total + chunk data   |

### State Flags (PONG response)

| Bit | Name       | Description                        |
|-----|------------|------------------------------------|
| 0   | READY      | Script Manager initialized, can accept EXECUTE_SCRIPT |
| 1   | EXECUTING  | A script is currently running      |
| 2   | ERROR      | Last execution produced an error   |

---

## Key Offsets (from existing offsets.h)

| Symbol                       | Offset   | Used In      |
|------------------------------|----------|--------------|
| ScriptContext                | 0x440    | Phase 1.5    |
| ScriptContextIdentityLevel   | 0x2C0    | Phase 1.5    |
| ScriptContextActiveScript    | 0x2B0    | Phase 1.5    |
| Children                     | 0x70     | Phase 1      |
| ChildrenEnd                  | 0x78     | Phase 1      |
| Name                         | 0x98     | Phase 1      |
| Parent                       | 0x68     | Phase 1      |
| ClassDescriptor              | 0x18     | Route H      |
| ClassDescriptorToClassName   | 0x8      | Route H      |
| TaskSchedulerPointer         | 0x84A58E0| Phase 1      |
| JobStart                     | 0xC8     | Phase 1      |
| JobEnd                       | 0xD0     | Phase 1      |
| Job_Name                     | 0x18     | Phase 1      |
| FakeDataModelPointer         | 0x7E26978| Route H      |

### To Be Discovered (Phase 1 reverse engineering)

| Item                              | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| ScriptContext → lua_State* offset | Needed if direct lua_State* access required beyond lua_pcall capture |
| Heartbeat job function pointer offset | Exact offset within job struct for trampoline target |
| Script object bytecode format     | Validate ByteCode_Pointer/Size offsets for script fabrication |

---

## Files to Create

| File                                          | Phase | Purpose                                      |
|-----------------------------------------------|-------|----------------------------------------------|
| `src/Injector/ManualMapInjector.h`            | 2     | PE parser, section writer, IAT resolver       |
| `src/Injector/ManualMapInjector.cpp`          | 2     | Implementation                                |
| `src/Injector/CapcomDriver.h`                 | 2     | Capcom IOCTL wrapper                          |
| `src/Injector/CapcomDriver.cpp`               | 2     | Implementation                                |
| `src/Injector/KernelExec.h`                   | 2     | Kernel function execution primitive           |
| `src/Injector/KernelExec.cpp`                 | 2     | Implementation                                |
| `src/Payload/PayloadDLL.cpp`                  | 1     | Injected DLL: VEH, pipe client, heartbeat hijack |
| `src/Payload/PayloadDLL.h`                    | 1     | Header                                        |
| `src/Payload/PipeProtocol.h`                  | 1     | Pipe frame structs and command constants      |
| `src/Payload/ScriptManager.lua`               | 1     | Embedded Lua Script Manager (injected as byte array) |
| `src/Protocol/PipeServer.h`                   | 3     | Named pipe server in UniversalHub             |
| `src/Protocol/PipeServer.cpp`                 | 3     | Implementation                                |
| `src/Hypervisor/EPTEngine.h`                  | 4     | EPT split engine (HyperPlatform-based)        |
| `src/Hypervisor/EPTEngine.cpp`                | 4     | Implementation                                |
| `src/Hypervisor/ShadowPageTable.h`            | 4     | Dual EPT view management                      |
| `src/Hypervisor/ShadowPageTable.cpp`          | 4     | Implementation                                |

## CMakeLists.txt Changes

- `src/CMakeLists.txt`: Add `PayloadDLL` shared library target (links Luau.VM, Luau.Compiler)
- `src/CMakeLists.txt`: Add `Injector` static library target
- `src/CMakeLists.txt`: Link `ManualMapInjector`, `CapcomDriver`, `PipeServer` to UniversalHub
- `src/CMakeLists.txt`: Phase 4: Add `Hypervisor` static library target (conditional on VT-x availability)

---

## Security & Educational Notice

This design is for **educational demonstration in controlled offline environments only.** The techniques described — kernel driver exploitation (BYOVD), manual DLL mapping, hardware breakpoint interception, and EPT hypervisor page splitting — are documented for understanding modern anti-cheat bypass architectures. Use only against processes you own, in isolated VM environments, for learning purposes.
