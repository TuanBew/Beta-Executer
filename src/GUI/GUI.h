/**
 * GUI.h — Dear ImGui / GLFW Control Panel
 *
 * Creates a standalone window with a Dear ImGui docking interface.
 * The GUI reads widget descriptions from LuaBridge's queue and renders
 * them as interactive ImGui controls (sliders, checkboxes, buttons,
 * color pickers, text, and log panels).
 *
 * Tabbed layout:
 *   - General      — Process info, attach/detach controls
 *   - Objects      — Object tree browser, communication object scanner
 *   - Visuals      — Toggles and parameters for visual overlays
 *   - Automation   — Movement, camera, environment sliders
 *   - Console      — Lua script editor and execution log
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include <unordered_map>

// Forward declarations
struct GLFWwindow;
struct GuiWidget;

/**
 * GUI singleton — manages the GLFW window and ImGui rendering loop.
 *
 * On each frame:
 *  1. Poll GLFW events
 *  2. Start ImGui frame
 *  3. Consume widget queue from LuaBridge
 *  4. Render docked panels
 *  5. Call LuaBridge::OnFrame() for module lifecycle
 *  6. Render ImGui + swap buffers
 */
class GUI {
public:
    static GUI& GetInstance();

    /**
     * Initialize the GLFW window and ImGui context.
     * @param title     Window title.
     * @param width     Initial width in pixels.
     * @param height    Initial height in pixels.
     * @return true if initialization succeeded.
     */
    bool Initialize(const std::string& title, int width = 1280, int height = 720);

    /**
     * Cleanup ImGui and GLFW.
     */
    void Shutdown();

    /**
     * Run the main render loop. Blocks until the window is closed.
     */
    void Run();

    /**
     * Request the main loop to stop (called when window close is requested).
     */
    void RequestStop();

    /**
     * @return true if the GUI is currently running.
     */
    bool IsRunning() const;

    /**
     * Set the hotkey for toggling GUI visibility (default: INSERT = 0x2D).
     * Pass 0 to disable toggle hotkey.
     */
    void SetToggleHotkey(int vkCode);

    /**
     * @return true if the GUI panel is currently visible.
     */
    bool IsVisible() const;

private:
    GUI() = default;
    ~GUI();
    GUI(const GUI&) = delete;
    GUI& operator=(const GUI&) = delete;

    // ---- Frame callbacks ----
    static void GlfwErrorCallback(int error, const char* description);

    // ---- Rendering helpers ----
    void RenderMenuBar();
    void RenderGeneralTab();
    void RenderObjectsTab();
    void RenderVisualsTab();
    void RenderAutomationTab();
    void RenderConsoleTab();
    void RenderLogPanel();

    // ---- Widget consumption ----
    void ProcessWidgetQueue();

    // ---- Lua console ----
    void ExecuteLuaInput();

    // ---- Layout persistence ----
    void SetupDockingLayout();
    void SaveConfig();
    void LoadConfig();

    // ---- GLFW / ImGui state ----
    GLFWwindow* m_window = nullptr;
    bool m_running = false;
    bool m_visible = true;
    int  m_toggleHotkey = 0x2D; // VK_INSERT
    bool m_hotkeyWasPressed = false;
    bool m_showDemoWindow = false;
    bool m_showMetrics = false;

    // ---- Console state ----
    char m_luaInputBuffer[4096] = {};
    std::vector<std::string> m_consoleHistory;
    int m_historyPos = -1;

    // ---- Tab state ----
    struct ObjectEntry {
        std::string name;
        std::string className;
        uintptr_t address;
    };
    std::vector<ObjectEntry> m_cachedObjects;
    bool m_objectsNeedRefresh = true;

    struct RemoteEntry {
        std::string name;
        std::string type;
        uintptr_t address;
    };
    std::vector<RemoteEntry> m_cachedRemotes;

    // ---- Slider state cache ----
    // Persists slider values across frames (widgets are re-queued each frame)
    struct SliderState {
        float value = 0.0f;
        int callbackRef = -1;
    };
    std::unordered_map<std::string, SliderState> m_sliderStates;

    // ---- Checkbox state cache ----
    struct CheckboxState {
        bool checked = false;
        int callbackRef = -1;
    };
    std::unordered_map<std::string, CheckboxState> m_checkboxStates;

    // ---- Timing ----
    std::chrono::steady_clock::time_point m_lastFrameTime;
    float m_deltaTime = 0.0f;
};
