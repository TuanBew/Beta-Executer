# Universal Hub — Beta-Executer

**Process interaction and scripting framework for educational demonstration.**

**Status:** 🗃️ **ARCHIVED** — Project concluded. See [Project Conclusion](docs/superpowers/specs/2026-08-03-project-conclusion.md) for full details.

---

## Overview

Universal Hub is a C++17 desktop application that attaches to a target process, performs memory read/write using internal structure offsets, and executes LuaU scripts through a bridge layer. It features a Dear ImGui control panel, a LuaU sandbox, privilege elevation, PE manual mapping, and a kernel driver interface (BYOVD).

**Built for a Dev-Challenge educational demonstration.** All code runs in controlled offline environments against a mock test target.

### Architecture (5 Layers)

```
┌─────────────────────────────────────────────────┐
│  Layer 1: GUI (Dear ImGui + GLFW)               │
│  4-tab interface: Executer | Injector |          │
│  Logger | Objects                                │
├─────────────────────────────────────────────────┤
│  Layer 2: Logging (Logger + CrashHandler)        │
│  3 sinks: File (rotation) | GUI (ring buffer) |  │
│  Debug (OutputDebugStringA)                      │
├─────────────────────────────────────────────────┤
│  Layer 3: Core Engine                            │
│  Process attach/detach, Memory<T> R/W,           │
│  Module base resolution, Bootstrap demo,         │
│  Privilege elevation (4 strategies)              │
├─────────────────────────────────────────────────┤
│  Layer 4: LuaU VM (LuaBridge)                    │
│  ~20 bridge functions, sandbox,                  │
│  Widget queue protocol, intercepts               │
├─────────────────────────────────────────────────┤
│  Layer 5: Injection Engine                       │
│  Manual map (kernel + user-mode),                │
│  3-tier execution pipeline,                      │
│  CapcomDriver kernel R/W                         │
└─────────────────────────────────────────────────┘
```

---

## Quick Start

### Prerequisites

- Windows 10+ x64
- Visual Studio 2022 (17.x) with C++ Desktop workload
- CMake 3.20+
- Git

### Build

```powershell
# Clone
git clone https://github.com/TuanBew/Beta-Executer.git
cd Beta-Executer

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build (Debug)
cmake --build build --config Debug

# Output: build/src/Debug/UniversalHub.exe
#         build/src/Debug/PayloadDLL.dll
#         build/src/Debug/NullDLL.dll
#         build/test-target/Debug/mock_test_exe.exe
```

### Test with Mock Target

```powershell
# Terminal 1: Launch mock target
.\build\test-target\Debug\mock_test_exe.exe
# Note the PID from console output

# Terminal 2: Inject payload
.\build\src\Debug\UniversalHub.exe --attach <PID> --inject usermap --bootstrap NullDLL.dll
```

### GUI Mode

```powershell
# Launch GUI (default mode)
.\build\src\Debug\UniversalHub.exe

# Or attach on startup
.\build\src\Debug\UniversalHub.exe --attach <PID>
```

**Controls:**
- `INSERT` key: toggle GUI visibility
- Four tabs: Executer | Injector | Logger | Objects
- Code editor: multi-tab, clipboard copy, resizable splitters

---

## Dependencies

| Library | Version | Source | Link Type |
|---------|---------|--------|-----------|
| LuaU | 0.663 | `luau-lang/luau` (FetchContent) | Static |
| Dear ImGui | docking branch | `ocornut/imgui` (FetchContent) | Header+impl |
| GLFW | 3.4 | `glfw/glfw` (FetchContent) | Static |
| nlohmann/json | 3.11.3 | `nlohmann/json` (FetchContent) | Single header |
| dbghelp.lib | Windows SDK | System | Dynamic |

---

## Injection Modes

| Flag | Mode | Description |
|------|------|-------------|
| `--inject legacy` | CreateRemoteThread | Standard `LoadLibraryA` injection (blocked by anti-cheat) |
| `--inject manual` | Kernel Manual Map | PE mapping via CapcomDriver kernel R/W + APC entry |
| `--inject usermap` | User-Mode Manual Map | PE mapping via RPM/WPM + 3-tier execution fallback |

### Three-Tier Execution Pipeline (`usermap`)

```
1. Thread Hijack → SetThreadContext redirect RIP to shellcode
     ↓ (blocked: RIP outside module range)
2. APC Injection → QueueUserAPC + PostThreadMessage + NtAlertThread
     ↓ (blocked: Hyperion filters non-module APCs)
3. Code-Cave APC → Write shellcode into module padding via kernel R/W
     ↓ (blocked by machine constraint: Capcom.sys won't load)
```

See [Project Conclusion](docs/superpowers/specs/2026-08-03-project-conclusion.md) for the full analysis of why each tier fails against Hyperion/Byfron.

---

## Key CLI Flags

```
--attach <PID|name>    Auto-attach to process on startup
--no-elevate           Skip privilege elevation (faster testing)
--inject <mode>        Injection mode: legacy | manual | usermap
--bootstrap <dll>      Auto-inject DLL after attach
--console              Legacy REPL mode (no GUI)
--run-script <file>    Execute Lua script via pipe
--run-pipe-script <f>  Execute Lua script via pipe connection
```

---

## Project Structure

```
src/
├── main.cpp                  # Entry point, CLI, Capcom load, auto-bootstrap
├── Core/                     # Engine, Memory<T>, Bootstrap, PrivilegeElevation, offsets.h
├── Injector/                 # CapcomDriver, KernelExec, ManualMapInjector, UserModeMapper
├── Logging/                  # Logger (3 sinks), CrashHandler (SEH+minidump)
├── Lua/                      # LuaBridge (LuaU VM + ~20 bridge functions)
├── GUI/                      # Dear ImGui + GLFW control panel
└── Payload/                  # PayloadDLL.cpp (pipe client), null_dll.cpp (heartbeat test)

scripts/                      # Lua scripts: universal_hub.lua, level_check.lua, etc.
test-target/                  # Mock test.exe with simulated memory layout
docs/superpowers/             # Specs, plans, iteration reports, project conclusion
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [Project Conclusion](docs/superpowers/specs/2026-08-03-project-conclusion.md) | Full project retrospective: what was built, what worked, why it failed, lessons learned |
| [Iteration Report](docs/superpowers/specs/2026-08-01-universal-hub-iteration-report.md) | Phase 1–14 completion report with verification checklist |
| [Architecture Spec](docs/superpowers/specs/2026-08-01-universal-hub-design.md) | Original design document (Phase 1) |
| [Ring-0 Executor Spec](docs/superpowers/specs/2026-08-02-ring0-executor-design.md) | Phase 2–4 kernel executor architecture |
| [Phase 1 Plan](docs/superpowers/plans/2026-08-02-phase1-implementation-plan.md) | Phase 1 implementation plan |
| [Phase 2 Plan](docs/superpowers/plans/2026-08-02-phase2-ring0-executor.md) | Phase 2 ring-0 executor plan |
| [Logging Design](docs/superpowers/specs/2026-08-01-logging-crash-system-design.md) | Logger + CrashHandler specification |

---

## Known Limitations

1. **Hyperion/Byfron blocks all execution** — project cannot inject into live Roblox without kernel driver
2. **Capcom.sys unavailable** — development machine cannot load kernel driver (no VM)
3. **Single-threaded** — GUI, Lua, memory I/O all on main thread
4. **No syntax highlighting** — code editor is plain text
5. **Hardcoded offsets** — internal structure offsets tied to specific target versions

---

## License & Disclaimer

**FOR EDUCATIONAL DEMONSTRATION ONLY.** This project was built for a Dev-Challenge to demonstrate C++ systems programming concepts: PE format parsing, process interaction APIs, kernel driver interfaces, Lua VM embedding, and GUI development. It is designed for use against the included mock test target (`test-target/mock_test_exe.exe`) in controlled offline environments. Use against live processes may violate terms of service.
