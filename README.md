# Universal Hub

**Educational Process Interaction & Automation Framework**

A C++17 desktop application demonstrating process-level memory interaction, embedded LuaU scripting, and modular GUI-based automation control — built as an educational Dev-Challenge project for controlled offline environments.

> **FOR EDUCATIONAL DEMONSTRATION ONLY** — designed for use with a mock test target in a controlled offline environment.

## Features

- **Process Interaction** — Attach to a target process and perform memory read/write operations using Win32 APIs (ReadProcessMemory, WriteProcessMemory)
- **LuaU Scripting** — Embedded Luau VM with an extensive bridge API exposing memory I/O, object traversal, and GUI binding to Lua scripts
- **Dear ImGui Control Panel** — Docking-based GUI with tabs for General, Objects, Visuals, Automation, and Console
- **Modular Lua Payload** — Config-driven module system with Object Highlight, Movement, FOV, Environment, and Communication Logger modules
- **Mock Test Target** — Companion executable that simulates the expected memory layout for safe testing

## Architecture

```
┌─────────────────────────┐
│   GUI Layer (ImGui)     │  ← INSERT to toggle, docked tabs
├─────────────────────────┤
│   Core Engine           │  ← Process attach, memory I/O, bootstrap
├─────────────────────────┤
│   LuaU VM (LuaBridge)   │  ← 20 bridge functions, sandbox, modules
├─────────────────────────┤
│   Lua Scripts           │  ← Config manager, feature modules
└─────────────────────────┘
```

Single-threaded — main loop drives ImGui rendering, Lua executes synchronously.

## Project Structure

```
Beta-Executer/
├── CMakeLists.txt                     # Root CMake (FetchContent deps)
├── README.md
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp                       # Entry point
│   ├── Core/
│   │   ├── offsets.h                  # ~400 internal structure offsets
│   │   ├── Engine.h / Engine.cpp      # Process attach/detach
│   │   ├── Memory.h                   # Read<T>/Write<T>/ReadString
│   │   └── Bootstrap.h / .cpp         # CreateRemoteThread demo
│   ├── Lua/
│   │   └── LuaBridge.h / .cpp         # LuaU VM + 20 bridge functions
│   └── GUI/
│       └── GUI.h / .cpp               # Dear ImGui + GLFW window
├── scripts/
│   └── universal_hub.lua              # Modular Lua payload
├── config/
│   └── default_config.json            # Default settings
├── docs/
│   └── superpowers/specs/             # Design documents
└── test-target/
    ├── CMakeLists.txt
    └── main.cpp                       # Mock test.exe
```

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| [Luau](https://github.com/luau-lang/luau) | 0.663 | Embedded scripting runtime |
| [Dear ImGui](https://github.com/ocornut/imgui) | docking | GUI framework |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | Window creation |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | JSON serialization |

All fetched automatically via CMake FetchContent.

## Building

### Prerequisites
- Windows 10/11
- Visual Studio 2022 with C++17 support
- CMake 3.20+
- Git

### Build Steps

```powershell
# Clone
git clone https://github.com/TuanBew/Beta-Executer.git
cd Beta-Executer

# Configure
cmake -B build -G "Visual Studio 17 2022" -A x64

# Build
cmake --build build --config Release
```

The build produces two executables:
- `build\src\Release\UniversalHub.exe` — main application
- `build\test-target\Release\mock_test_exe.exe` — mock test target

### Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TEST_TARGET` | `ON` | Build the mock test.exe |

## Usage

### 1. Start the mock test target

```powershell
.\build\test-target\Release\mock_test_exe.exe
```

Note the PID printed on startup.

### 2. Launch Universal Hub

```powershell
# Interactive mode
.\build\src\Release\UniversalHub.exe

# Or attach immediately
.\build\src\Release\UniversalHub.exe --attach <PID>
```

### 3. Console Commands (Phase 1)

```
  attach <process>   Attach by name or PID
  detach             Detach from process
  read <addr> <type> Read memory (i32, f32, u8, str)
  write <addr> <val> Write memory
  module <name>      Get module base address
  bootstrap <dll>    Educational DLL loading demo
  gui                Launch GUI control panel
  help               Show help
  exit               Quit
```

### 4. GUI Control Panel

Press **INSERT** to toggle the GUI. Tabs:

- **General** — Process status, attach/detach, bootstrap controls
- **Objects** — Object tree browser, communication object scanner
- **Visuals** — Object highlight toggles and color pickers
- **Automation** — Walk speed, jump power, FOV, gravity, brightness sliders
- **Console** — Lua script editor with live execution

### 5. Lua Scripting

Load and execute Lua scripts through the Console tab or automatically via `scripts/universal_hub.lua`. The bridge API exposes:

```lua
-- Memory operations
read_memory(address, "f32")       -- read typed value
write_memory(address, 16.0, "f32") -- write typed value
read_string(address, 256)          -- read string

-- Object traversal
get_object_children(parent)        -- walk Children linked list
find_first_child(parent, "Head")   -- search by name
get_class_name(object)             -- resolve class via descriptor

-- Communication objects
get_remote_events()                -- find RemoteEvent/RemoteFunction

-- GUI binding
gui_add_tab("My Tab")
gui_add_slider("My Tab", "Speed", 0, 100, 16, function(v) print(v) end)
gui_add_checkbox("My Tab", "Enable", false, function(v) print(v) end)
gui_add_button("My Tab", "Click Me", function() print("clicked!") end)
gui_log("Hello from Lua!")

-- File I/O & JSON
read_file("config.json")
write_file("config.json", data)
json_decode('{"key": "value"}')
json_encode({key = "value"})
```

## Verification Checklist

1. [ ] Build mock target, run it, note PID
2. [ ] Build UniversalHub, attach to mock target PID
3. [ ] Press INSERT → GUI appears
4. [ ] Verify each tab renders with widgets
5. [ ] Execute Lua in console: `read_memory(0x..., "f32")`
6. [ ] Toggle walk speed slider → verify value changes in mock target
7. [ ] Scan communication objects → verify entries appear
8. [ ] Save config → close → reopen → settings restored

## License

Educational project — not for production use.

---

**FOR EDUCATIONAL DEMONSTRATION ONLY** — intended for use with `test.exe` in a controlled offline environment.
