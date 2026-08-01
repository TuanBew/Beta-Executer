# Universal Hub — Design Document

**Date:** 2026-08-01
**Repository:** https://github.com/TuanBew/Beta-Executer.git
**Branch:** main

---

## Context

Build a C++ desktop application that attaches to a target process (`test.exe`), performs high-performance memory read/write using known internal structure offsets, and executes LuaU scripts to drive automation. The tool includes a Dear ImGui control panel for live parameter adjustment. This is an educational demonstration project for a Dev-Challenge — used only in controlled offline environments with a mock test target.

**Sanitization:** All language must use neutral technical terms (automation framework, process interaction, scripting runtime, communication object). No references to game exploitation, cheating, or platform-specific APIs.

---

## Project Structure

```
Beta-Executer/
├── CMakeLists.txt                       # Root CMake with FetchContent deps
├── README.md
├── .gitignore
├── docs/
│   └── superpowers/specs/
│       └── 2026-08-01-universal-hub-design.md  # This document
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp                         # Entry point
│   ├── Core/
│   │   ├── offsets.h                    # All 250+ offsets verbatim
│   │   ├── Engine.h / Engine.cpp        # Process attach/detach
│   │   ├── Memory.h                     # Templated Read<T>/Write<T>
│   │   └── Bootstrap.h / Bootstrap.cpp  # Optional CreateRemoteThread demo
│   ├── Lua/
│   │   └── LuaBridge.h / LuaBridge.cpp  # LuaU VM + bridge functions
│   └── GUI/
│       └── GUI.h / GUI.cpp              # Dear ImGui + GLFW window
├── scripts/
│   └── universal_hub.lua                # Modular Lua payload
├── config/
│   └── default_config.json
└── test-target/
    ├── CMakeLists.txt
    └── main.cpp                         # Mock test.exe
```

---

## Dependencies (CMake FetchContent)

| Library       | Source                         | Notes                                                 |
| ------------- | ------------------------------ | ----------------------------------------------------- |
| LuaU          | luau-lang/luau (0.663)         | Static library — Luau.VM, Luau.Compiler, Luau.Config |
| Dear ImGui    | ocornut/imgui (docking branch) | Header-only + impl .cpp files                         |
| GLFW          | glfw/glfw (3.4)                | Static library                                        |
| nlohmann/json | nlohmann/json (v3.11.3)        | Single header `#include <nlohmann/json.hpp>`        |

---

## Architecture

Four layers, top-down:

1. **GUI Layer** — GLFW window + Dear ImGui docking. Captures INSERT hotkey to toggle. Renders tabs dynamically populated by Lua scripts via the GUI binding protocol.
2. **Core Engine** — Process attachment (CreateToolhelp32Snapshot + OpenProcess), memory I/O (ReadProcessMemory/WriteProcessMemory), module base resolution. Optional bootstrap via CreateRemoteThread + LoadLibraryA (educational demo only).
3. **LuaU VM (LuaBridge)** — Initializes `lua_State*`, registers bridge functions as globals, sandboxes the environment. All bridge functions call into Core Engine.
4. **Lua Scripting Layer** — `universal_hub.lua` with modular feature tables (init/update/render lifecycle). Auto-detects communication objects, manages config JSON, drives GUI population.

**Thread model:** Single-threaded — main loop runs GLFW + ImGui render, Lua executes synchronously, memory ops are brief blocking calls.

---

## Lua Bridge API

### Memory Operations

- `read_memory(address, type)` — read typed value from target
- `write_memory(address, value, type)` — write typed value
- `read_string(address, maxLen)` — read null-terminated UTF-8

### Object Traversal

- `get_object_children(parentAddress)` — walk Children linked list (offset 0x70)
- `find_first_child(parentAddress, name)` — search by Name (offset 0x98)
- `get_class_name(objectAddress)` — resolve via ClassDescriptor chain

### Communication Objects

- `get_remote_events()` — scan for RemoteEvent/RemoteFunction objects

### Function Interception

- `intercept_function(objectAddress, callback)` — educational detour demo

### GUI Binding Protocol

- `gui_add_tab(name)`, `gui_add_slider(...)`, `gui_add_checkbox(...)`, `gui_add_button(...)`, `gui_add_color_picker(...)`, `gui_add_text(...)`, `gui_log(message)`

### File I/O & JSON

- `read_file(filename)`, `write_file(filename, content)`
- `json_decode(json_string)`, `json_encode(table)`

---

## Implementation Phases

### Phase 1: Core Engine ✅ COMPLETE

- [X] Engine singleton: `AttachToProcess` / `Detach`
- [X] Memory templates: `Read<T>`, `Write<T>`, `ReadString`
- [X] `GetModuleBaseAddress`
- [X] Bootstrap demo (optional, educational)
- [X] Console-based unit test against mock target
- [X] Mock test target (`test-target/`)

### Phase 2: LuaU Integration 🔄 IN PROGRESS

- [X] LuaBridge.h (header with class declaration, GuiWidget, InterceptHandle)
- [ ] **LuaBridge.cpp** — VM initialization, register all ~20 bridge functions as C closures, sandboxing, ExecuteString/ExecuteFile, OnFrame, widget queue, intercept management
- [ ] Update `src/CMakeLists.txt` to include LuaBridge.cpp

### Phase 3: Dear ImGui GUI

- [ ] GUI.h / GUI.cpp — GLFW window, docking context
- [ ] Tabbed layout (General, Objects, Visuals, Automation, Settings)
- [ ] INSERT hotkey toggle
- [ ] Widget queue consumption from LuaBridge
- [ ] Update `src/CMakeLists.txt` to include GUI.cpp

### Phase 4: Lua Script & Modules

- [ ] `scripts/universal_hub.lua` — config manager, module system
- [ ] Feature modules: Object Highlight, Movement, FOV, Communication Logger, Environment
- [ ] GUI binding protocol usage

### Phase 5: Configuration & Persistence

- [ ] Bridge `read_file`/`write_file`
- [ ] Auto-save on close, load on startup
- [ ] Default config fallback

### Phase 6: Integration & Testing

- [ ] Mock test.exe integration
- [ ] Full walkthrough of all features
- [ ] Config round-trip test
- [ ] README.md

---

## Mock Test Target (test-target/)

A companion executable that allocates a memory region matching the expected layout:

- 16MB VirtualAlloc region with known base address
- Sample objects with Name strings, Children linked lists, ClassDescriptor chains
- Writable fields: WalkSpeed (0x1d0), FOV (0x140), Health (0x190), etc.
- RemoteEvent/RemoteFunction objects for scanner testing
- Prints PID and base address on startup for easy attachment

---

## Verification Checklist

1. [ ] Build mock target, run it, note PID/base
2. [ ] Build UniversalHub, attach to mock target PID
3. [ ] Press INSERT → GUI appears
4. [ ] Verify each tab renders, sliders/checkboxes are responsive
5. [ ] Memory dump confirms offset writes (WalkSpeed @ 0x1d0, FOV @ 0x140, etc.)
6. [ ] "Scan Remotes" populates Objects tab
7. [ ] Enable Communication Logger, trigger mock event → log entry appears
8. [ ] Save config → close → reopen → settings restored
9. [ ] All bridge functions return correct types (no Lua errors)
1. [ ] GUI remains responsive during Lua execution
