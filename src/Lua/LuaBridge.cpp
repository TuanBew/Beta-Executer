/**
 * LuaBridge.cpp — LuaU VM Integration & Bridge Functions
 *
 * Initializes the LuaU virtual machine, registers all bridge functions as
 * Lua globals, applies sandboxing, and provides the ExecuteString/ExecuteFile
 * interface. The bridge functions allow Lua scripts to interact with the
 * target process through the Core Engine (memory I/O, object traversal,
 * communication object scanning, GUI binding, file I/O, and function
 * interception).
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#include "LuaBridge.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unordered_set>
#include <nlohmann/json.hpp>

// LuaU / Luau headers
#include <lua.h>
#include <lualib.h>
#include <luacode.h>

#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/offsets.h"

// ---- Forward Declarations of Bridge C Functions ----

// Memory operations
static int l_read_memory(lua_State* L);
static int l_write_memory(lua_State* L);
static int l_read_string(lua_State* L);

// Object traversal
static int l_get_object_children(lua_State* L);
static int l_find_first_child(lua_State* L);
static int l_get_class_name(lua_State* L);

// Communication objects
static int l_get_remote_events(lua_State* L);

// Function interception
static int l_intercept_function(lua_State* L);

// GUI binding protocol
static int l_gui_add_tab(lua_State* L);
static int l_gui_add_slider(lua_State* L);
static int l_gui_add_checkbox(lua_State* L);
static int l_gui_add_button(lua_State* L);
static int l_gui_add_color_picker(lua_State* L);
static int l_gui_add_text(lua_State* L);
static int l_gui_log(lua_State* L);

// File I/O & JSON
static int l_read_file(lua_State* L);
static int l_write_file(lua_State* L);
static int l_json_decode(lua_State* L);
static int l_json_encode(lua_State* L);

// Utility
static int l_get_module_base(lua_State* L);

// ---- Table of Bridge Functions to Register ----

static const luaL_Reg kBridgeFunctions[] = {
    // Memory
    {"read_memory",         l_read_memory},
    {"write_memory",        l_write_memory},
    {"read_string",         l_read_string},

    // Object traversal
    {"get_object_children", l_get_object_children},
    {"find_first_child",    l_find_first_child},
    {"get_class_name",      l_get_class_name},

    // Communication objects
    {"get_remote_events",   l_get_remote_events},

    // Function interception
    {"intercept_function",  l_intercept_function},

    // GUI binding
    {"gui_add_tab",         l_gui_add_tab},
    {"gui_add_slider",      l_gui_add_slider},
    {"gui_add_checkbox",    l_gui_add_checkbox},
    {"gui_add_button",      l_gui_add_button},
    {"gui_add_color_picker",l_gui_add_color_picker},
    {"gui_add_text",        l_gui_add_text},
    {"gui_log",             l_gui_log},

    // File I/O & JSON
    {"read_file",           l_read_file},
    {"write_file",          l_write_file},
    {"json_decode",         l_json_decode},
    {"json_encode",         l_json_encode},

    // Utility
    {"get_module_base",     l_get_module_base},

    {nullptr, nullptr}
};

// ---- Safe Standard Library Subset ----

// Functions we allow from the standard Lua libraries
static const char* kSafeGlobals[] = {
    // Basic
    "assert", "error", "ipairs", "next", "pairs", "pcall",
    "select", "tonumber", "tostring", "type", "unpack",
    "xpcall", "rawequal", "rawget", "rawset", "setmetatable",
    "getmetatable",

    // Math (subset)
    "math_abs", "math_ceil", "math_floor", "math_max", "math_min",
    "math_sqrt", "math_pi", "math_huge",

    // String (subset)
    "string_byte", "string_char", "string_find", "string_format",
    "string_gmatch", "string_gsub", "string_len", "string_lower",
    "string_match", "string_rep", "string_reverse", "string_sub",
    "string_upper", "string_split",

    // Table
    "table_insert", "table_remove", "table_sort", "table_concat",
    "table_find", "table_create", "table_freeze", "table_isfrozen",

    // Coroutine-aware
    "coroutine_create", "coroutine_resume", "coroutine_status",
    "coroutine_wrap", "coroutine_yield",

    nullptr
};

// ---- Singleton Access ----

LuaBridge& LuaBridge::GetInstance() {
    static LuaBridge instance;
    return instance;
}

LuaBridge::~LuaBridge() {
    Shutdown();
}

// ============================================================
//  Initialize / Shutdown
// ============================================================

bool LuaBridge::Initialize() {
    if (L) {
        return true; // Already initialized
    }

    // Create a new LuaU VM
    L = luaL_newstate();
    if (!L) {
        std::cerr << "[LuaBridge] Failed to create lua_State" << std::endl;
        return false;
    }

    // Open all standard libraries (we'll sandbox afterward)
    luaL_openlibs(L);

    // Apply sandboxing before registering bridge functions
    ApplySandbox();

    // Register all bridge functions as globals
    RegisterBridgeFunctions();

    std::cout << "[LuaBridge] LuaU VM initialized successfully" << std::endl;
    return true;
}

void LuaBridge::Shutdown() {
    if (L) {
        lua_close(L);
        L = nullptr;
        m_intercepts.clear();
        m_widgetQueue.clear();
        std::cout << "[LuaBridge] LuaU VM shut down" << std::endl;
    }
}

// ============================================================
//  Sandboxing
// ============================================================

void LuaBridge::ApplySandbox() {
    // Strategy: Create a new global table that contains only the safe
    // standard library subset, then set it as the global environment.

    // 1. Create a new table to serve as the sandboxed global environment
    lua_newtable(L);  // [new_globals]

    // 2. Copy safe standard library functions from the original globals
    lua_pushglobaltable(L);  // [new_globals, old_globals]

    for (int i = 0; kSafeGlobals[i] != nullptr; ++i) {
        const char* name = kSafeGlobals[i];

        // Names like "math_abs" map to math.abs in the original table
        // We need to traverse the path and copy the value
        // For simplicity, handle the underscore notation:
        // "math_abs" → old_globals["math"]["abs"]
        std::string fullName(name);
        size_t underscore = fullName.find('_');

        if (underscore != std::string::npos) {
            // Dotted path: e.g. math.abs
            std::string lib = fullName.substr(0, underscore);     // "math"
            std::string func = fullName.substr(underscore + 1);   // "abs"

            // Push the library table
            lua_pushstring(L, lib.c_str());  // [ng, og, "math"]
            if (lua_rawget(L, -2) == LUA_TTABLE) {
                // [ng, og, math_table]
                lua_pushstring(L, func.c_str());  // [ng, og, math_table, "abs"]
                if (lua_rawget(L, -2) == LUA_TFUNCTION) {
                    // [ng, og, math_table, func]
                    // Store in new globals as math_abs (or we could rebuild
                    // the nested table structure)
                    lua_pushstring(L, name);        // [ng, og, mt, func, "math_abs"]
                    lua_pushvalue(L, -2);           // [ng, og, mt, func, "math_abs", func]
                    lua_rawset(L, -6);              // [ng, og, mt, func]
                }
                lua_pop(L, 2);  // pop func (or nil) + math_table
            } else {
                lua_pop(L, 1);  // pop non-table
            }
        } else {
            // Simple global: e.g. "assert", "ipairs"
            lua_pushstring(L, name);           // [ng, og, "assert"]
            if (lua_rawget(L, -2) == LUA_TFUNCTION) {
                // [ng, og, func]
                lua_pushstring(L, name);        // [ng, og, func, "assert"]
                lua_pushvalue(L, -2);           // [ng, og, func, "assert", func]
                lua_rawset(L, -5);              // [ng, og, func]
            }
            lua_pop(L, 1);  // pop func (or nil)
        }
    }

    lua_pop(L, 1);  // pop old globals — [new_globals]

    // 3. Copy safe essential fields
    lua_pushstring(L, "_VERSION");
    lua_pushstring(L, "Luau 0.663 (sandboxed)");
    lua_rawset(L, -3);

    // 4. Set the sandboxed table as the global environment
    // In Luau, we use lua_setglobal for the __env mechanism via luaL_sandbox
    // For simplicity, we replace the global table reference
    lua_pushvalue(L, -1);          // [new_globals, new_globals]
    lua_setglobal(L, "_G");        // _G = new_globals; [new_globals]

    // Use Luau sandbox API if available (provides time/memory limits)
    // luaL_sandbox(L, 1000000); // 1M instructions limit

    lua_pop(L, 1);  // clean stack
}

// ============================================================
//  Register Bridge Functions
// ============================================================

void LuaBridge::RegisterBridgeFunctions() {
    // Push our bridge functions into the global table
    lua_pushglobaltable(L);

    for (const luaL_Reg* reg = kBridgeFunctions; reg->name != nullptr; ++reg) {
        lua_pushcfunction(L, reg->func, reg->name);
        lua_setfield(L, -2, reg->name);
    }

    lua_pop(L, 1);  // pop global table
}

// ============================================================
//  ExecuteString / ExecuteFile
// ============================================================

bool LuaBridge::ExecuteString(const std::string& code) {
    if (!L) {
        std::cerr << "[LuaBridge] VM not initialized" << std::endl;
        return false;
    }

    // Compile Lua source to bytecode using Luau compiler
    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(code.data(), code.size(), nullptr, &bytecodeSize);

    if (!bytecode) {
        std::cerr << "[LuaBridge] Compilation failed" << std::endl;
        return false;
    }

    // Load bytecode onto the Lua stack
    int loadResult = luau_load(L, "=string", bytecode, bytecodeSize, 0);
    std::free(bytecode);  // luau_compile allocates with malloc

    if (loadResult != 0) {
        // Error string is on the stack
        const char* err = lua_tostring(L, -1);
        ReportError(std::string("Load error: ") + (err ? err : "unknown"));
        lua_pop(L, 1);
        return false;
    }

    // Execute the loaded function (protected call)
    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        ReportError(std::string("Runtime error: ") + (err ? err : "unknown"));
        lua_pop(L, 1);
        return false;
    }

    return true;
}

bool LuaBridge::ExecuteFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[LuaBridge] Cannot open file: " << path << std::endl;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string code = buffer.str();
    file.close();

    // Set the chunk name to the file path for better error messages
    // We need to compile with a custom name — use ExecuteString approach
    // but with the file path embedded in the compiled chunk name

    if (!L) {
        std::cerr << "[LuaBridge] VM not initialized" << std::endl;
        return false;
    }

    size_t bytecodeSize = 0;
    char* bytecode = luau_compile(code.data(), code.size(), nullptr, &bytecodeSize);

    if (!bytecode) {
        std::cerr << "[LuaBridge] Compilation failed for: " << path << std::endl;
        return false;
    }

    int loadResult = luau_load(L, path.c_str(), bytecode, bytecodeSize, 0);
    std::free(bytecode);

    if (loadResult != 0) {
        const char* err = lua_tostring(L, -1);
        ReportError(std::string("Load error in ") + path + ": " + (err ? err : "unknown"));
        lua_pop(L, 1);
        return false;
    }

    if (lua_pcall(L, 0, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        ReportError(std::string("Runtime error in ") + path + ": " + (err ? err : "unknown"));
        lua_pop(L, 1);
        return false;
    }

    std::cout << "[LuaBridge] Executed script: " << path << std::endl;
    return true;
}

// ============================================================
//  OnFrame — Module Lifecycle
// ============================================================

void LuaBridge::OnFrame() {
    if (!L) return;

    // Call the global on_frame() function if it exists
    lua_getglobal(L, "on_frame");
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            std::cerr << "[LuaBridge] on_frame error: " << (err ? err : "unknown") << std::endl;
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);  // pop non-function
    }
}

// ============================================================
//  GUI Widget Queue
// ============================================================

void LuaBridge::QueueWidget(const GuiWidget& widget) {
    m_widgetQueue.push_back(widget);
}

std::vector<GuiWidget> LuaBridge::FlushWidgetQueue() {
    std::vector<GuiWidget> result;
    result.swap(m_widgetQueue);
    return result;
}

// ============================================================
//  Intercept Management
// ============================================================

InterceptHandle* LuaBridge::CreateIntercept(uintptr_t address, int callbackRef) {
    InterceptHandle handle{};
    handle.targetAddress = address;
    handle.luaCallbackRef = callbackRef;
    handle.active = true;
    m_intercepts.push_back(handle);
    return &m_intercepts.back();
}

void LuaBridge::RemoveIntercept(uintptr_t address) {
    m_intercepts.erase(
        std::remove_if(m_intercepts.begin(), m_intercepts.end(),
                       [address](const InterceptHandle& h) {
                           return h.targetAddress == address;
                       }),
        m_intercepts.end());
}

// ============================================================
//  Accessors
// ============================================================

lua_State* LuaBridge::GetState() const {
    return L;
}

void LuaBridge::ReportError(const std::string& context) {
    m_lastError = context;
    std::cerr << "[LuaBridge] " << context << std::endl;
}

// ============================================================
//  Bridge Function Implementations
// ============================================================

// ---------------------------- Helpers ----------------------------

/**
 * Resolve a typed value from the Lua stack at the given index.
 * Used by read/write bridge functions.
 */
static Memory::TypeCode ResolveTypeCode(const char* typeStr) {
    if (!typeStr) return Memory::TypeCode::Float;

    if (std::strcmp(typeStr, "i32") == 0 || std::strcmp(typeStr, "int32") == 0)
        return Memory::TypeCode::Int32;
    if (std::strcmp(typeStr, "f32") == 0 || std::strcmp(typeStr, "float") == 0)
        return Memory::TypeCode::Float;
    if (std::strcmp(typeStr, "f64") == 0 || std::strcmp(typeStr, "double") == 0)
        return Memory::TypeCode::Double;
    if (std::strcmp(typeStr, "i64") == 0 || std::strcmp(typeStr, "int64") == 0)
        return Memory::TypeCode::Int64;
    if (std::strcmp(typeStr, "u8") == 0 || std::strcmp(typeStr, "uint8") == 0)
        return Memory::TypeCode::Uint8;
    if (std::strcmp(typeStr, "bool") == 0)
        return Memory::TypeCode::Bool;
    if (std::strcmp(typeStr, "i16") == 0 || std::strcmp(typeStr, "int16") == 0)
        return Memory::TypeCode::Int16;
    if (std::strcmp(typeStr, "u32") == 0 || std::strcmp(typeStr, "uint32") == 0)
        return Memory::TypeCode::Uint32;

    return Memory::TypeCode::Float; // default
}

/**
 * Push a value to the Lua stack based on type code.
 */
static void PushTypedValue(lua_State* L, Memory::TypeCode tc, uintptr_t addr) {
    switch (tc) {
        case Memory::TypeCode::Int32:
            lua_pushinteger(L, static_cast<int>(Memory::Read<int32_t>(addr)));
            break;
        case Memory::TypeCode::Float:
            lua_pushnumber(L, static_cast<double>(Memory::Read<float>(addr)));
            break;
        case Memory::TypeCode::Double:
            lua_pushnumber(L, Memory::Read<double>(addr));
            break;
        case Memory::TypeCode::Int64:
            lua_pushinteger(L, static_cast<lua_Integer>(Memory::Read<int64_t>(addr)));
            break;
        case Memory::TypeCode::Uint8:
            lua_pushinteger(L, static_cast<int>(Memory::Read<uint8_t>(addr)));
            break;
        case Memory::TypeCode::Bool:
            lua_pushboolean(L, Memory::Read<uint8_t>(addr) != 0);
            break;
        case Memory::TypeCode::Int16:
            lua_pushinteger(L, static_cast<int>(Memory::Read<int16_t>(addr)));
            break;
        case Memory::TypeCode::Uint32:
            lua_pushinteger(L, static_cast<lua_Integer>(Memory::Read<uint32_t>(addr)));
            break;
        default:
            lua_pushnil(L);
            break;
    }
}

/**
 * Write a value from the Lua stack based on type code.
 */
static void WriteTypedValue(lua_State* L, Memory::TypeCode tc, uintptr_t addr) {
    // The value is at stack index specified by caller
    // We define this as a helper used inline by l_write_memory
    // (not used directly — see l_write_memory implementation)
}

// ---------------------------- Memory Operations ----------------------------

/**
 * read_memory(address, type) → value
 *
 * Read a typed value from the target process.
 * type: "i32", "f32", "f64", "i64", "u8", "bool", "i16", "u32"
 */
static int l_read_memory(lua_State* L) {
    lua_Integer address = luaL_checkinteger(L, 1);
    const char* typeStr = luaL_optstring(L, 2, "f32");

    try {
        Memory::TypeCode tc = ResolveTypeCode(typeStr);
        PushTypedValue(L, tc, static_cast<uintptr_t>(address));
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

/**
 * write_memory(address, value, type)
 *
 * Write a typed value to the target process.
 */
static int l_write_memory(lua_State* L) {
    lua_Integer address = luaL_checkinteger(L, 1);
    const char* typeStr = luaL_optstring(L, 3, "f32");

    try {
        Memory::TypeCode tc = ResolveTypeCode(typeStr);
        uintptr_t addr = static_cast<uintptr_t>(address);

        switch (tc) {
            case Memory::TypeCode::Int32:
                Memory::Write<int32_t>(addr, static_cast<int32_t>(luaL_checkinteger(L, 2)));
                break;
            case Memory::TypeCode::Float:
                Memory::Write<float>(addr, static_cast<float>(luaL_checknumber(L, 2)));
                break;
            case Memory::TypeCode::Double:
                Memory::Write<double>(addr, static_cast<double>(luaL_checknumber(L, 2)));
                break;
            case Memory::TypeCode::Int64:
                Memory::Write<int64_t>(addr, static_cast<int64_t>(luaL_checkinteger(L, 2)));
                break;
            case Memory::TypeCode::Uint8:
                Memory::Write<uint8_t>(addr, static_cast<uint8_t>(luaL_checkinteger(L, 2)));
                break;
            case Memory::TypeCode::Bool: {
                luaL_checktype(L, 2, LUA_TBOOLEAN);
                Memory::Write<uint8_t>(addr, lua_toboolean(L, 2) ? 1 : 0);
                break;
            }
            case Memory::TypeCode::Int16:
                Memory::Write<int16_t>(addr, static_cast<int16_t>(luaL_checkinteger(L, 2)));
                break;
            case Memory::TypeCode::Uint32:
                Memory::Write<uint32_t>(addr, static_cast<uint32_t>(luaL_checkinteger(L, 2)));
                break;
            default:
                lua_pushboolean(L, false);
                lua_pushstring(L, "Unknown type code");
                return 2;
        }

        lua_pushboolean(L, true);
        return 1;
    } catch (const std::exception& e) {
        lua_pushboolean(L, false);
        lua_pushstring(L, e.what());
        return 2;
    }
}

/**
 * read_string(address, maxLen) → string
 *
 * Read a null-terminated string from the target process.
 */
static int l_read_string(lua_State* L) {
    lua_Integer address = luaL_checkinteger(L, 1);
    lua_Integer maxLen = luaL_optinteger(L, 2, 256);

    try {
        std::string str = Memory::ReadString(static_cast<uintptr_t>(address),
                                             static_cast<size_t>(maxLen));
        lua_pushstring(L, str.c_str());
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

// ---------------------------- Object Traversal ----------------------------

/**
 * get_object_children(parentAddress) → {child1, child2, ...}
 *
 * Walk the Children linked list starting at parent+0x70.
 * Each node's "next" pointer is at offset 0x8.
 * Returns a table of child base addresses.
 */
static int l_get_object_children(lua_State* L) {
    lua_Integer parentAddr = luaL_checkinteger(L, 1);
    uintptr_t addr = static_cast<uintptr_t>(parentAddr);

    try {
        // Read Children pointer at parent + offsets::Children (0x70)
        uintptr_t firstChild = Memory::Read<uintptr_t>(addr + offsets::Children);

        lua_newtable(L);
        int index = 1;

        uintptr_t current = firstChild;
        const int kMaxChildren = 1000; // Safety limit

        for (int i = 0; i < kMaxChildren && current != 0; ++i) {
            lua_pushinteger(L, static_cast<lua_Integer>(current));
            lua_rawseti(L, -2, index++);

            // Next sibling at offset 0x8 (ChildrenEnd)
            current = Memory::Read<uintptr_t>(current + offsets::ChildrenEnd);
        }

        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

/**
 * find_first_child(parentAddress, name) → address | nil
 *
 * Search the Children list for the first child whose Name (0x98) matches.
 */
static int l_find_first_child(lua_State* L) {
    lua_Integer parentAddr = luaL_checkinteger(L, 1);
    const char* searchName = luaL_checkstring(L, 2);

    try {
        uintptr_t addr = static_cast<uintptr_t>(parentAddr);
        uintptr_t firstChild = Memory::Read<uintptr_t>(addr + offsets::Children);

        uintptr_t current = firstChild;
        const int kMaxSearch = 1000;

        for (int i = 0; i < kMaxSearch && current != 0; ++i) {
            std::string childName = Memory::ReadString(current + offsets::Name);
            if (childName == searchName) {
                lua_pushinteger(L, static_cast<lua_Integer>(current));
                return 1;
            }
            current = Memory::Read<uintptr_t>(current + offsets::ChildrenEnd);
        }

        lua_pushnil(L);  // Not found
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

/**
 * get_class_name(objectAddress) → string
 *
 * Resolve the class name of an object by following:
 *   object + ClassDescriptor (0x18) → ClassDescriptor
 *   ClassDescriptor + ClassDescriptorToClassName (0x8) → class name string
 */
static int l_get_class_name(lua_State* L) {
    lua_Integer objectAddr = luaL_checkinteger(L, 1);

    try {
        uintptr_t addr = static_cast<uintptr_t>(objectAddr);

        // Read ClassDescriptor pointer at object+0x18
        uintptr_t cd = Memory::Read<uintptr_t>(addr + offsets::ClassDescriptor);
        if (cd == 0) {
            lua_pushstring(L, "(no class descriptor)");
            return 1;
        }

        // Read class name string pointer at ClassDescriptor+0x8
        uintptr_t classNamePtr = Memory::Read<uintptr_t>(cd + offsets::ClassDescriptorToClassName);
        if (classNamePtr == 0) {
            lua_pushstring(L, "(no class name pointer)");
            return 1;
        }

        std::string className = Memory::ReadString(classNamePtr);
        lua_pushstring(L, className.c_str());
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

// ---------------------------- Communication Objects ----------------------------

/**
 * get_remote_events() → {{name, address, type}, ...}
 *
 * Scans the object tree for communication objects (RemoteEvent, RemoteFunction).
 * Starts from the workspace (found via DataModel → Workspace pointer chain)
 * or from the target's base address + FakeDataModel offset as fallback.
 */
static int l_get_remote_events(lua_State* L) {
    try {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            lua_pushnil(L);
            lua_pushstring(L, "Not attached to a process");
            return 2;
        }

        lua_newtable(L);
        int index = 1;

        // Approach: Walk the object tree from DataModel, looking for
        // objects whose class name contains "RemoteEvent" or "RemoteFunction"
        //
        // For the mock target, we can use the FakeDataModel pointer
        // at known offset or walk from module base.

        uintptr_t workspace = 0;

        // Try via FakeDataModel → FakeDataModelToDataModel → Workspace
        uintptr_t base = engine.GetModuleBase();
        if (base != 0) {
            uintptr_t fakeDM = Memory::Read<uintptr_t>(base + offsets::FakeDataModelPointer);
            if (fakeDM != 0) {
                uintptr_t realDM = Memory::Read<uintptr_t>(fakeDM + offsets::FakeDataModelToDataModel);
                if (realDM != 0) {
                    workspace = Memory::Read<uintptr_t>(realDM + offsets::Workspace);
                }
            }
        }

        // Fallback: try to find workspace via Children of DataModel
        // (simplified — full tree walk would be expensive)

        // For now, return what we can find. In a real session, the Lua
        // script would use get_object_children and get_class_name to walk.
        // We scan a limited area from the module base for objects with
        // class name "RemoteEvent" or "RemoteFunction"

        if (workspace != 0) {
            uintptr_t firstChild = Memory::Read<uintptr_t>(workspace + offsets::Children);
            uintptr_t current = firstChild;
            const int kMaxScan = 5000;

            for (int i = 0; i < kMaxScan && current != 0; ++i) {
                try {
                    uintptr_t cd = Memory::Read<uintptr_t>(current + offsets::ClassDescriptor);
                    if (cd != 0) {
                        uintptr_t cnPtr = Memory::Read<uintptr_t>(cd + offsets::ClassDescriptorToClassName);
                        if (cnPtr != 0) {
                            std::string cn = Memory::ReadString(cnPtr, 64);
                            if (cn == "RemoteEvent" || cn == "RemoteFunction") {
                                // Push a table with name, address, type
                                lua_newtable(L);

                                lua_pushstring(L, "name");
                                std::string objName = Memory::ReadString(current + offsets::Name);
                                lua_pushstring(L, objName.c_str());
                                lua_rawset(L, -3);

                                lua_pushstring(L, "address");
                                lua_pushinteger(L, static_cast<lua_Integer>(current));
                                lua_rawset(L, -3);

                                lua_pushstring(L, "type");
                                lua_pushstring(L, cn.c_str());
                                lua_rawset(L, -3);

                                lua_rawseti(L, -2, index++);
                            }
                        }
                    }
                    current = Memory::Read<uintptr_t>(current + offsets::ChildrenEnd);
                } catch (...) {
                    // Skip objects that fail to read
                    current = Memory::Read<uintptr_t>(current + offsets::ChildrenEnd);
                }
            }
        }

        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

// ---------------------------- Function Interception ----------------------------

/**
 * intercept_function(objectAddress, callback) → handleId
 *
 * Register a Lua callback to be invoked when a function at the given
 * address is intercepted. Returns a numeric handle for removal.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */
static int l_intercept_function(lua_State* L) {
    lua_Integer targetAddr = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // Store the callback in the Lua registry and get a reference
    lua_pushvalue(L, 2);
    int callbackRef = lua_ref(L, LUA_REGISTRYINDEX);

    auto& bridge = LuaBridge::GetInstance();
    InterceptHandle* handle = bridge.CreateIntercept(
        static_cast<uintptr_t>(targetAddr), callbackRef);

    if (handle) {
        lua_pushinteger(L, static_cast<lua_Integer>(handle->targetAddress));
    } else {
        lua_pushnil(L);
    }

    return 1;
}

// ---------------------------- GUI Binding Protocol ----------------------------

/**
 * gui_add_tab(name)
 *
 * Create a new tab in the GUI control panel.
 */
static int l_gui_add_tab(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    GuiWidget widget{};
    widget.type = GuiWidget::Type::Tab;
    widget.tabName = name;
    widget.label = name;

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

/**
 * gui_add_slider(tab, label, min, max, default, callback)
 *
 * Add a float slider to a tab. callback is called with the new value.
 */
static int l_gui_add_slider(lua_State* L) {
    const char* tab = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);
    float minVal = static_cast<float>(luaL_checknumber(L, 3));
    float maxVal = static_cast<float>(luaL_checknumber(L, 4));
    float defaultVal = static_cast<float>(luaL_optnumber(L, 5, minVal));

    GuiWidget widget{};
    widget.type = GuiWidget::Type::Slider;
    widget.tabName = tab;
    widget.label = label;
    widget.minVal = minVal;
    widget.maxVal = maxVal;
    widget.values[0] = defaultVal;

    // Store callback in registry if provided
    if (lua_gettop(L) >= 6 && lua_isfunction(L, 6)) {
        lua_pushvalue(L, 6);
        widget.callbackRef = lua_ref(L, LUA_REGISTRYINDEX);
    }

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

/**
 * gui_add_checkbox(tab, label, default, callback)
 *
 * Add a checkbox toggle to a tab.
 */
static int l_gui_add_checkbox(lua_State* L) {
    const char* tab = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);
    bool defaultVal = false;
    if (lua_gettop(L) >= 3) {
        luaL_checktype(L, 3, LUA_TBOOLEAN);
        defaultVal = lua_toboolean(L, 3);
    }

    GuiWidget widget{};
    widget.type = GuiWidget::Type::Checkbox;
    widget.tabName = tab;
    widget.label = label;
    widget.values[0] = defaultVal ? 1.0f : 0.0f;

    if (lua_gettop(L) >= 4 && lua_isfunction(L, 4)) {
        lua_pushvalue(L, 4);
        widget.callbackRef = lua_ref(L, LUA_REGISTRYINDEX);
    }

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

/**
 * gui_add_button(tab, label, callback)
 *
 * Add a clickable button to a tab.
 */
static int l_gui_add_button(lua_State* L) {
    const char* tab = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);

    GuiWidget widget{};
    widget.type = GuiWidget::Type::Button;
    widget.tabName = tab;
    widget.label = label;

    if (lua_gettop(L) >= 3 && lua_isfunction(L, 3)) {
        lua_pushvalue(L, 3);
        widget.callbackRef = lua_ref(L, LUA_REGISTRYINDEX);
    }

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

/**
 * gui_add_color_picker(tab, label, r, g, b, a, callback)
 *
 * Add a color picker to a tab. Defaults to white (1,1,1,1).
 */
static int l_gui_add_color_picker(lua_State* L) {
    const char* tab = luaL_checkstring(L, 1);
    const char* label = luaL_checkstring(L, 2);

    GuiWidget widget{};
    widget.type = GuiWidget::Type::ColorPicker;
    widget.tabName = tab;
    widget.label = label;
    widget.values[0] = static_cast<float>(luaL_optnumber(L, 3, 1.0)); // R
    widget.values[1] = static_cast<float>(luaL_optnumber(L, 4, 1.0)); // G
    widget.values[2] = static_cast<float>(luaL_optnumber(L, 5, 1.0)); // B
    widget.values[3] = static_cast<float>(luaL_optnumber(L, 6, 1.0)); // A

    if (lua_gettop(L) >= 7 && lua_isfunction(L, 7)) {
        lua_pushvalue(L, 7);
        widget.callbackRef = lua_ref(L, LUA_REGISTRYINDEX);
    }

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

/**
 * gui_add_text(tab, text)
 *
 * Display static text in a tab.
 */
static int l_gui_add_text(lua_State* L) {
    const char* tab = luaL_checkstring(L, 1);
    const char* text = luaL_checkstring(L, 2);

    GuiWidget widget{};
    widget.type = GuiWidget::Type::Text;
    widget.tabName = tab;
    widget.text = text;

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

/**
 * gui_log(message)
 *
 * Append a message to the GUI log panel.
 */
static int l_gui_log(lua_State* L) {
    const char* message = luaL_checkstring(L, 1);

    GuiWidget widget{};
    widget.type = GuiWidget::Type::Log;
    widget.text = message;

    LuaBridge::GetInstance().QueueWidget(widget);
    return 0;
}

// ---------------------------- File I/O & JSON ----------------------------

/**
 * read_file(filename) → string
 *
 * Read the entire contents of a file. Path is relative to the scripts/
 * directory or absolute.
 */
static int l_read_file(lua_State* L) {
    const char* filename = luaL_checkstring(L, 1);

    std::ifstream file(filename);
    if (!file.is_open()) {
        // Try relative to scripts/ directory
        std::string altPath = "scripts/" + std::string(filename);
        file.open(altPath);
        if (!file.is_open()) {
            lua_pushnil(L);
            lua_pushstring(L, "Cannot open file");
            return 2;
        }
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    lua_pushstring(L, content.c_str());
    return 1;
}

/**
 * write_file(filename, content)
 *
 * Write content to a file. Creates the file if it doesn't exist.
 */
static int l_write_file(lua_State* L) {
    const char* filename = luaL_checkstring(L, 1);
    const char* content = luaL_checkstring(L, 2);

    std::ofstream file(filename);
    if (!file.is_open()) {
        lua_pushboolean(L, false);
        lua_pushstring(L, "Cannot open file for writing");
        return 2;
    }

    file << content;
    file.close();

    lua_pushboolean(L, true);
    return 1;
}

/**
 * json_decode(json_string) → table
 *
 * Parse a JSON string into a Lua table.
 * Uses nlohmann/json for parsing.
 */
static int l_json_decode(lua_State* L) {
    const char* jsonStr = luaL_checkstring(L, 1);

    try {
        nlohmann::json parsed = nlohmann::json::parse(jsonStr);

        // Convert JSON → Lua table (recursive helper via lambda)
        std::function<void(const nlohmann::json&)> pushJsonValue;
        pushJsonValue = [&](const nlohmann::json& j) {
            switch (j.type()) {
                case nlohmann::json::value_t::null:
                    lua_pushnil(L);
                    break;
                case nlohmann::json::value_t::boolean:
                    lua_pushboolean(L, j.get<bool>());
                    break;
                case nlohmann::json::value_t::number_integer:
                case nlohmann::json::value_t::number_unsigned:
                    lua_pushinteger(L, j.get<lua_Integer>());
                    break;
                case nlohmann::json::value_t::number_float:
                    lua_pushnumber(L, j.get<double>());
                    break;
                case nlohmann::json::value_t::string:
                    lua_pushstring(L, j.get<std::string>().c_str());
                    break;
                case nlohmann::json::value_t::array:
                    lua_newtable(L);
                    int arrIdx;
                    arrIdx = 1;
                    for (const auto& elem : j) {
                        pushJsonValue(elem);
                        lua_rawseti(L, -2, arrIdx++);
                    }
                    break;
                case nlohmann::json::value_t::object:
                    lua_newtable(L);
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        pushJsonValue(it.value());
                        lua_setfield(L, -2, it.key().c_str());
                    }
                    break;
                default:
                    lua_pushnil(L);
                    break;
            }
        };

        pushJsonValue(parsed);
        return 1;
    } catch (const nlohmann::json::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

/**
 * json_encode(table) → string
 *
 * Serialize a Lua table to a JSON string.
 */
static int l_json_encode(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    try {
        // Convert Lua table → JSON (recursive)
        std::function<nlohmann::json(int)> luaToJson;
        luaToJson = [&](int idx) -> nlohmann::json {
            int t = lua_type(L, idx);
            switch (t) {
                case LUA_TNIL:
                    return nullptr;
                case LUA_TBOOLEAN:
                    return lua_toboolean(L, idx) != 0;
                case LUA_TNUMBER:
                    if (lua_isinteger(L, idx)) {
                        return lua_tointeger(L, idx);
                    }
                    return lua_tonumber(L, idx);
                case LUA_TSTRING:
                    return lua_tostring(L, idx);
                case LUA_TTABLE: {
                    // Check if array or object
                    bool isArray = true;
                    lua_pushnil(L);
                    while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0) {
                        if (lua_type(L, -2) != LUA_TNUMBER ||
                            lua_tointeger(L, -2) < 1) {
                            isArray = false;
                        }
                        lua_pop(L, 1);
                    }

                    if (isArray) {
                        nlohmann::json arr = nlohmann::json::array();
                        lua_pushnil(L);
                        while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0) {
                            arr.push_back(luaToJson(lua_gettop(L)));
                            lua_pop(L, 1);
                        }
                        return arr;
                    } else {
                        nlohmann::json obj = nlohmann::json::object();
                        lua_pushnil(L);
                        while (lua_next(L, idx < 0 ? idx - 1 : idx) != 0) {
                            const char* key = lua_tostring(L, -2);
                            if (key) {
                                obj[key] = luaToJson(lua_gettop(L));
                            }
                            lua_pop(L, 1);
                        }
                        return obj;
                    }
                }
                default:
                    return nullptr;
            }
        };

        nlohmann::json result = luaToJson(1);
        std::string jsonStr = result.dump(2);  // Pretty-print with 2-space indent
        lua_pushstring(L, jsonStr.c_str());
        return 1;
    } catch (const std::exception& e) {
        lua_pushnil(L);
        lua_pushstring(L, e.what());
        return 2;
    }
}

// ---------------------------- Utility ----------------------------

/**
 * get_module_base(moduleName) → address
 *
 * Get the base address of a loaded module in the target process.
 */
static int l_get_module_base(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);

    uintptr_t base = Memory::GetModuleBaseAddress(moduleName);
    if (base != 0) {
        lua_pushinteger(L, static_cast<lua_Integer>(base));
    } else {
        lua_pushnil(L);
        lua_pushstring(L, "Module not found");
        return 2;
    }

    return 1;
}
