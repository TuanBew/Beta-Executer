#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

// Forward-declare LuaU types (from luau/lua.h)
struct lua_State;

/**
 * LuaBridge — embeds a LuaU VM and exposes bridge functions that enable
 * Lua scripts to interact with the target process through the Core Engine.
 *
 * All bridge functions are registered as global Lua functions. The Lua
 * environment is sandboxed: only the registered bridge functions and a
 * safe subset of standard Lua libraries are available.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

// ---- GUI Widget Description ----
// When a Lua script calls gui_add_* functions, widget descriptions are
// queued here. The GUI render loop consumes them each frame.

struct GuiWidget {
    enum class Type {
        Tab,
        Slider,
        Checkbox,
        Button,
        ColorPicker,
        Text,
        Log
    };

    Type type;
    std::string tabName;
    std::string label;
    float minVal = 0.0f;
    float maxVal = 1.0f;
    float values[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // Slider: [current, _, _, _], Color: [r,g,b,a]
    int callbackRef = -1; // Lua registry reference for the callback function
    std::string text;     // For Text and Log types
};

// ---- Function Interception Handle ----
struct InterceptHandle {
    uintptr_t targetAddress;
    int luaCallbackRef;
    bool active;
};

class LuaBridge {
public:
    static LuaBridge& GetInstance();

    /**
     * Initialize the LuaU VM. Must be called after Engine is set up.
     * Registers all bridge functions as globals and sets up sandboxing.
     */
    bool Initialize();

    /**
     * Close the LuaU VM and release all resources.
     */
    void Shutdown();

    /**
     * Execute a Lua code string.
     * @param code  Lua source code to execute.
     * @return true if execution succeeded (no errors).
     */
    bool ExecuteString(const std::string& code);

    /**
     * Execute a Lua script file.
     * @param path  Path to the .lua file.
     * @return true if execution succeeded.
     */
    bool ExecuteFile(const std::string& path);

    /**
     * Get the raw lua_State for direct C API access (used by GUI).
     */
    lua_State* GetState() const;

    // ---- Module lifecycle ----
    // Called each frame from the main loop to invoke module update/render

    /**
     * Call the global on_frame() Lua function (invokes all module update/render).
     */
    void OnFrame();

    // ---- GUI Widget Queue ----
    // Lua bridge functions push widgets here; the GUI reads them.

    void QueueWidget(const GuiWidget& widget);
    std::vector<GuiWidget> FlushWidgetQueue();

    // ---- Intercept Management ----
    InterceptHandle* CreateIntercept(uintptr_t address, int callbackRef);
    void RemoveIntercept(uintptr_t address);

private:
    LuaBridge() = default;
    ~LuaBridge();
    LuaBridge(const LuaBridge&) = delete;
    LuaBridge& operator=(const LuaBridge&) = delete;

    /**
     * Register all bridge functions as Lua globals.
     */
    void RegisterBridgeFunctions();

    /**
     * Sandbox the Lua environment: remove dangerous standard library
     * functions and restrict to bridge-only capabilities.
     */
    void ApplySandbox();

    /**
     * Log a Lua error to the console.
     */
    void ReportError(const std::string& context);

    lua_State* L = nullptr;
    std::vector<GuiWidget> m_widgetQueue;
    std::vector<InterceptHandle> m_intercepts;
    std::string m_lastError;
};
