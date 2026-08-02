/**
 * GUI.h — Dear ImGui / GLFW Control Panel
 *
 * Creates a standalone window with a Dear ImGui interface.
 * The GUI reads widget descriptions from LuaBridge's queue and renders
 * them as interactive ImGui controls (sliders, checkboxes, buttons,
 * color pickers, text, and log panels).
 *
 * Single-window layout with tab navigation:
 *   - Executer  — Multi-tab code editor + script manager + output + toolbar
 *   - Injector  — Process attach/detach, bootstrap, privilege elevation
 *   - Logger    — Filtered event log
 *   - Objects   — Object tree browser, communication object scanner
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>

struct GLFWwindow;
struct GuiWidget;

class GUI {
public:
    static GUI& GetInstance();
    bool Initialize(const std::string& title, int width = 1280, int height = 720);
    void Shutdown();
    void Run();
    void RequestStop();
    bool IsRunning() const;
    void SetToggleHotkey(int vkCode);
    bool IsVisible() const;

private:
    GUI() = default;
    ~GUI();
    GUI(const GUI&) = delete;
    GUI& operator=(const GUI&) = delete;

    static void GlfwErrorCallback(int error, const char* description);

    void RenderMenuBar();
    void RenderInjectorTab();
    void RenderObjectsTab();
    void RenderExecuterTab();
    void RenderErrorLoggerTab();
    void RenderPopups();

    void ProcessWidgetQueue();
    void ExecuteLuaInput();

    void SaveConfig();
    void LoadConfig();
    void RefreshScriptList();

    int  FindTabByPath(const std::string& path);
    void OpenScriptInTab(const std::string& filename);
    void RequestCloseTab(int index);
    void ForceCloseTab(int index);
    void SaveTab(int index);
    bool HasUnsavedTabs();
    void CreateNewTab();

    // ---- GLFW / ImGui state ----
    GLFWwindow* m_window = nullptr;
    bool m_running = false;
    bool m_visible = true;
    int  m_toggleHotkey = 0x2D; // VK_INSERT
    bool m_hotkeyWasPressed = false;
    bool m_showDemoWindow = false;
    bool m_showMetrics = false;

    // ---- Main tab navigation ----
    int m_activeTab = 0; // 0=Executer, 1=Injector, 2=Logger, 3=Objects

    // ---- Editor tab system ----
    struct EditorTab {
        std::string name;
        std::string filePath;
        std::vector<char> buffer;
        bool isDirty = false;

        EditorTab(const std::string& n = "Untitled", const std::string& fp = "")
            : name(n), filePath(fp), buffer(16384, '\0') {}
    };
    std::vector<EditorTab> m_editorTabs;
    int m_activeEditorTab = 0;
    int m_untitledCounter = 1;

    // ---- Script panel ----
    char m_scriptSearchFilter[128] = {};
    std::vector<std::string> m_scriptFiles;
    int m_contextMenuScript = -1;

    // ---- Resizable splitter state ----
    float m_editorOutputSplit = 0.78f;
    float m_scriptPanelWidth = 200.0f;

    // ---- Console history ----
    std::vector<std::string> m_consoleHistory;
    int m_historyPos = -1;

    // ---- Popup state ----
    bool m_showSaveConfirm = false;
    bool m_showNamePopup = false;
    bool m_showDeleteConfirm = false;
    bool m_showRenamePopup = false;
    int  m_popupTargetTab = -1;
    int  m_popupTargetScript = -1;
    bool m_closingApp = false;
    char m_nameBuffer[256] = {};

    // ---- Injector (privilege) state ----
    int  m_privilegeTargetLevel = 10;
    bool m_privilegeAutoElevate = true;
    bool m_privilegeBypassChecks = true;

    // ---- Objects tab state ----
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

    // ---- Timing ----
    std::chrono::steady_clock::time_point m_lastFrameTime;
    float m_deltaTime = 0.0f;
};
