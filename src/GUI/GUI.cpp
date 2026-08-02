/**
 * GUI.cpp — Dear ImGui / GLFW Control Panel Implementation
 *
 * Single-window layout with tab navigation, amber accent theme,
 * multi-tab code editor with VS-like save/close behavior.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#define NOMINMAX
#include "GUI.h"

#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include "Logging/Logger.h"
#include "Core/PrivilegeElevation.h"
#include "Core/Engine.h"
#include "Core/Memory.h"
#include "Core/Bootstrap.h"
#include "Core/offsets.h"
#include "../Lua/LuaBridge.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>

// ============================================================
//  Theme Palette (Amber Accent)
// ============================================================

namespace Theme {
    static const ImVec4 Accent(0.878f, 0.596f, 0.188f, 1.0f);
    static const ImVec4 AccentDim(0.690f, 0.467f, 0.145f, 1.0f);
    static const ImVec4 Success(0.310f, 0.710f, 0.380f, 1.0f);
    static const ImVec4 Warning(0.878f, 0.682f, 0.220f, 1.0f);
    static const ImVec4 Error(0.820f, 0.310f, 0.310f, 1.0f);
    static const ImVec4 Fatal(0.940f, 0.250f, 0.250f, 1.0f);
    static const ImVec4 Dim(0.376f, 0.384f, 0.439f, 1.0f);
    static const ImVec4 Info(0.310f, 0.680f, 0.740f, 1.0f);
    static const ImVec4 Label(0.545f, 0.557f, 0.620f, 1.0f);
}

// ============================================================
//  Helper Functions
// ============================================================

static bool AccentButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.690f, 0.467f, 0.145f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.780f, 0.533f, 0.180f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.580f, 0.390f, 0.118f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

static bool GreenButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.176f, 0.463f, 0.255f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.216f, 0.545f, 0.302f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.145f, 0.388f, 0.212f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

static bool DangerButton(const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.502f, 0.188f, 0.188f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.600f, 0.235f, 0.235f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.420f, 0.153f, 0.153f, 1.0f));
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(3);
    return clicked;
}

static ImVec4 LogLevelColor(Logging::LogLevel level) {
    switch (level) {
        case Logging::LogLevel::Trace: return Theme::Dim;
        case Logging::LogLevel::Debug: return Theme::Info;
        case Logging::LogLevel::Info:  return ImVec4(0.78f, 0.79f, 0.82f, 1.0f);
        case Logging::LogLevel::Warn:  return Theme::Warning;
        case Logging::LogLevel::Error: return Theme::Error;
        case Logging::LogLevel::Fatal: return Theme::Fatal;
        default: return ImVec4(1, 1, 1, 1);
    }
}

static void Section(const char* label) {
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(Theme::Label, "%s", label);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.145f, 0.149f, 0.192f, 0.35f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4));
}

static std::string ShowOpenFileDialog() {
    char filename[MAX_PATH] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Lua Scripts (*.lua)\0*.lua\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "lua";
    ofn.lpstrTitle = "Open Script";

    if (GetOpenFileNameA(&ofn))
        return std::string(filename);
    return "";
}

// ============================================================
//  Singleton
// ============================================================

GUI& GUI::GetInstance() {
    static GUI instance;
    return instance;
}

GUI::~GUI() {
    Shutdown();
}

// ============================================================
//  Initialize / Shutdown
// ============================================================

void GUI::GlfwErrorCallback(int error, const char* description) {
    LOG_ERROR("[GUI] GLFW Error %d: %s", error, description);
}

bool GUI::Initialize(const std::string& title, int width, int height) {
    glfwSetErrorCallback(GlfwErrorCallback);

    if (!glfwInit()) {
        LOG_ERROR("[GUI] Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        LOG_ERROR("[GUI] Failed to create GLFW window");
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "config/imgui_v2.ini";

    // ---- Dark Amber Theme ----
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding     = ImVec2(8, 8);
    style.FramePadding      = ImVec2(8, 5);
    style.CellPadding       = ImVec2(6, 3);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.IndentSpacing     = 18;
    style.ScrollbarSize     = 12;
    style.GrabMinSize       = 8;
    style.WindowBorderSize  = 0;
    style.ChildBorderSize   = 1;
    style.PopupBorderSize   = 1;
    style.FrameBorderSize   = 0;
    style.TabBorderSize     = 0;
    style.WindowRounding    = 0;
    style.ChildRounding     = 4;
    style.FrameRounding     = 4;
    style.PopupRounding     = 4;
    style.ScrollbarRounding = 8;
    style.GrabRounding      = 3;
    style.TabRounding       = 4;
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);

    ImVec4* c = style.Colors;

    c[ImGuiCol_WindowBg]             = ImVec4(0.067f, 0.067f, 0.090f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.075f, 0.075f, 0.102f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.086f, 0.086f, 0.114f, 0.97f);
    c[ImGuiCol_Border]               = ImVec4(0.145f, 0.149f, 0.192f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_Text]                 = ImVec4(0.847f, 0.851f, 0.878f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.376f, 0.384f, 0.439f, 1.00f);
    c[ImGuiCol_FrameBg]              = ImVec4(0.098f, 0.102f, 0.137f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.122f, 0.125f, 0.165f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.141f, 0.145f, 0.188f, 1.00f);
    c[ImGuiCol_TitleBg]              = ImVec4(0.055f, 0.055f, 0.078f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.067f, 0.067f, 0.094f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.055f, 0.055f, 0.078f, 0.75f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.059f, 0.059f, 0.082f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.047f, 0.047f, 0.071f, 0.40f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.165f, 0.169f, 0.216f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.204f, 0.208f, 0.263f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.255f, 0.259f, 0.325f, 1.00f);
    c[ImGuiCol_CheckMark]            = ImVec4(0.878f, 0.596f, 0.188f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.878f, 0.596f, 0.188f, 0.80f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.940f, 0.650f, 0.220f, 1.00f);
    c[ImGuiCol_Button]               = ImVec4(0.133f, 0.137f, 0.176f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.169f, 0.173f, 0.216f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.110f, 0.114f, 0.149f, 1.00f);
    c[ImGuiCol_Header]               = ImVec4(0.133f, 0.137f, 0.176f, 0.55f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.169f, 0.173f, 0.216f, 0.70f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.192f, 0.196f, 0.247f, 0.80f);
    c[ImGuiCol_Separator]            = ImVec4(0.145f, 0.149f, 0.192f, 0.50f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.878f, 0.596f, 0.188f, 0.50f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.878f, 0.596f, 0.188f, 0.85f);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.878f, 0.596f, 0.188f, 0.15f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.878f, 0.596f, 0.188f, 0.50f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.878f, 0.596f, 0.188f, 0.85f);
    c[ImGuiCol_Tab]                  = ImVec4(0.071f, 0.071f, 0.098f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.149f, 0.153f, 0.200f, 0.80f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.098f, 0.098f, 0.133f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.878f, 0.596f, 0.188f, 0.80f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.059f, 0.059f, 0.082f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.086f, 0.090f, 0.118f, 1.00f);
    c[ImGuiCol_DockingPreview]       = ImVec4(0.878f, 0.596f, 0.188f, 0.35f);
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.055f, 0.055f, 0.078f, 1.00f);
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.098f, 0.102f, 0.137f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.145f, 0.149f, 0.192f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.118f, 0.122f, 0.161f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.000f, 1.000f, 1.000f, 0.02f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.878f, 0.596f, 0.188f, 0.22f);
    c[ImGuiCol_DragDropTarget]       = ImVec4(0.878f, 0.596f, 0.188f, 0.85f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.878f, 0.596f, 0.188f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    LoadConfig();
    RefreshScriptList();

    if (m_editorTabs.empty())
        CreateNewTab();

    m_running = true;
    m_lastFrameTime = std::chrono::steady_clock::now();

    LOG_INFO("[GUI] Initialized — %dx%d (INSERT to toggle)", width, height);
    return true;
}

void GUI::Shutdown() {
    if (!m_window) return;

    SaveConfig();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(m_window);
    glfwTerminate();
    m_window = nullptr;
    m_running = false;

    LOG_INFO("[GUI] Shut down");
}

// ============================================================
//  Main Loop
// ============================================================

void GUI::Run() {
    if (!m_window || !m_running) return;

    while (m_running) {
        glfwPollEvents();

        if (glfwWindowShouldClose(m_window)) {
            if (HasUnsavedTabs()) {
                glfwSetWindowShouldClose(m_window, GLFW_FALSE);
                m_closingApp = true;
                m_popupTargetTab = -1;
                m_showSaveConfirm = true;
            } else {
                break;
            }
        }

        auto now = std::chrono::steady_clock::now();
        m_deltaTime = std::chrono::duration<float>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;

        // INSERT hotkey toggle
        if (m_toggleHotkey != 0) {
            bool pressed = glfwGetKey(m_window, m_toggleHotkey) == GLFW_PRESS;
            if (pressed && !m_hotkeyWasPressed) {
                m_visible = !m_visible;
                LOG_INFO("[GUI] %s", m_visible ? "Shown" : "Hidden");
            }
            m_hotkeyWasPressed = pressed;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (m_visible) {
            RenderMenuBar();

            ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::Begin("##MainPanel", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::PopStyleVar(2);

            // Navigation tab bar
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 10));
            if (ImGui::BeginTabBar("##NavTabs", ImGuiTabBarFlags_None)) {
                if (ImGui::BeginTabItem("  Executer  "))  { m_activeTab = 0; ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("  Injector  "))  { m_activeTab = 1; ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("  Logger  "))    { m_activeTab = 2; ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("  Objects  "))   { m_activeTab = 3; ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::PopStyleVar();

            // Content area
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
            ImGui::BeginChild("##ContentArea", ImVec2(0, 0), false,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            ProcessWidgetQueue();

            switch (m_activeTab) {
                case 0: RenderExecuterTab();    break;
                case 1: RenderInjectorTab();    break;
                case 2: RenderErrorLoggerTab(); break;
                case 3: RenderObjectsTab();     break;
            }

            ImGui::EndChild();
            ImGui::PopStyleVar();

            RenderPopups();

            if (m_showDemoWindow) ImGui::ShowDemoWindow(&m_showDemoWindow);
            if (m_showMetrics) ImGui::ShowMetricsWindow(&m_showMetrics);

            ImGui::End(); // MainPanel
        }

        LuaBridge::GetInstance().OnFrame();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(m_window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.055f, 0.055f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(m_window);
    }

    Shutdown();
}

void GUI::RequestStop() {
    m_running = false;
}

bool GUI::IsRunning() const {
    return m_running;
}

void GUI::SetToggleHotkey(int vkCode) {
    m_toggleHotkey = vkCode;
}

bool GUI::IsVisible() const {
    return m_visible;
}

// ============================================================
//  Menu Bar
// ============================================================

void GUI::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Script", "Ctrl+N"))
                CreateNewTab();
            if (ImGui::MenuItem("Open Script...", "Ctrl+O")) {
                std::string path = ShowOpenFileDialog();
                if (!path.empty()) {
                    namespace fs = std::filesystem;
                    std::string name = fs::path(path).filename().string();
                    int existing = FindTabByPath(path);
                    if (existing >= 0) {
                        m_activeEditorTab = existing;
                    } else {
                        EditorTab tab(name, path);
                        std::ifstream f(path);
                        if (f.is_open()) {
                            std::string content((std::istreambuf_iterator<char>(f)),
                                                std::istreambuf_iterator<char>());
                            size_t len = (std::min)(content.size(), tab.buffer.size() - 1);
                            std::memcpy(tab.buffer.data(), content.c_str(), len);
                        }
                        m_editorTabs.push_back(std::move(tab));
                        m_activeEditorTab = (int)m_editorTabs.size() - 1;
                    }
                }
            }
            if (ImGui::MenuItem("Save Script", "Ctrl+S")) {
                if (!m_editorTabs.empty())
                    SaveTab(m_activeEditorTab);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save Config"))
                SaveConfig();
            if (ImGui::MenuItem("Load Config"))
                LoadConfig();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
                RequestStop();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Demo Window", nullptr, &m_showDemoWindow);
            ImGui::MenuItem("Metrics", nullptr, &m_showMetrics);
            ImGui::Separator();
            if (ImGui::MenuItem("Hide GUI", "INSERT"))
                m_visible = false;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("Universal Hub v1.0.0");
            ImGui::TextDisabled("Educational Process Interaction Framework");
            ImGui::EndMenu();
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 100.0f);
        ImGui::TextColored(Theme::Dim, "FPS: %.0f", ImGui::GetIO().Framerate);

        ImGui::EndMainMenuBar();
    }

    // Keyboard shortcuts
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        if (!m_editorTabs.empty())
            SaveTab(m_activeEditorTab);
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        CreateNewTab();
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        std::string path = ShowOpenFileDialog();
        if (!path.empty()) {
            namespace fs = std::filesystem;
            std::string name = fs::path(path).filename().string();
            int existing = FindTabByPath(path);
            if (existing >= 0) {
                m_activeEditorTab = existing;
            } else {
                EditorTab tab(name, path);
                std::ifstream f(path);
                if (f.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
                    size_t len = (std::min)(content.size(), tab.buffer.size() - 1);
                    std::memcpy(tab.buffer.data(), content.c_str(), len);
                }
                m_editorTabs.push_back(std::move(tab));
                m_activeEditorTab = (int)m_editorTabs.size() - 1;
            }
        }
    }
}

// ============================================================
//  Executer Tab — Multi-Tab Editor + Script Panel + Toolbar
// ============================================================

void GUI::RenderExecuterTab() {
    ImVec2 avail = ImGui::GetContentRegionAvail();

    const float toolbarH = 50.0f;
    const float splitterThick = 5.0f;
    const float toolbarPadX = 12.0f;
    const float minPanelW = 120.0f;
    const float minEditorH = 60.0f;
    const float minOutputH = 40.0f;

    float contentH = avail.y - toolbarH;

    // Clamp script panel width
    if (m_scriptPanelWidth < minPanelW) m_scriptPanelWidth = minPanelW;
    if (m_scriptPanelWidth > avail.x - minPanelW - splitterThick)
        m_scriptPanelWidth = avail.x - minPanelW - splitterThick;

    float leftW = avail.x - m_scriptPanelWidth - splitterThick;

    // ---- Left column: Editor tabs + code + output ----
    ImGui::BeginChild("##LeftCol", ImVec2(leftW, contentH), false);

    // Editor tab bar
    ImGuiTabBarFlags tabBarFlags = ImGuiTabBarFlags_AutoSelectNewTabs
                                 | ImGuiTabBarFlags_FittingPolicyScroll
                                 | ImGuiTabBarFlags_Reorderable;
    if (ImGui::BeginTabBar("##EditorTabs", tabBarFlags)) {
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            CreateNewTab();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("New Tab (Ctrl+N)");

        int tabToClose = -1;
        for (int i = 0; i < (int)m_editorTabs.size(); i++) {
            bool open = true;
            ImGuiTabItemFlags itemFlags = 0;
            if (m_editorTabs[i].isDirty)
                itemFlags |= ImGuiTabItemFlags_UnsavedDocument;

            std::string label = m_editorTabs[i].name + "##etab" + std::to_string(i);

            if (ImGui::BeginTabItem(label.c_str(), &open, itemFlags)) {
                m_activeEditorTab = i;
                ImGui::EndTabItem();
            }

            if (!open) {
                tabToClose = i;
            }
        }
        ImGui::EndTabBar();

        if (tabToClose >= 0)
            RequestCloseTab(tabToClose);
    }

    // Calculate editor/output split from ratio
    float innerH = ImGui::GetContentRegionAvail().y;
    float editorH = innerH * m_editorOutputSplit - splitterThick * 0.5f;
    float outputH = innerH * (1.0f - m_editorOutputSplit) - splitterThick * 0.5f;

    if (editorH < minEditorH) { editorH = minEditorH; outputH = innerH - editorH - splitterThick; }
    if (outputH < minOutputH) { outputH = minOutputH; editorH = innerH - outputH - splitterThick; }

    // Code editor for active tab
    if (!m_editorTabs.empty() && m_activeEditorTab >= 0 &&
        m_activeEditorTab < (int)m_editorTabs.size()) {
        auto& tab = m_editorTabs[m_activeEditorTab];

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.055f, 0.078f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.055f, 0.055f, 0.078f, 1.0f));
        ImGui::BeginChild("##EditorBg", ImVec2(-1, editorH), true);

        if (ImGui::InputTextMultiline("##luaEditor", tab.buffer.data(),
                                       tab.buffer.size(),
                                       ImVec2(-1, -1),
                                       ImGuiInputTextFlags_AllowTabInput)) {
            tab.isDirty = true;
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    // ---- Horizontal splitter (editor / output) ----
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.145f, 0.149f, 0.192f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::AccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Accent);
    ImGui::Button("##hsplit", ImVec2(-1, splitterThick));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.y;
        m_editorOutputSplit += delta / innerH;
        if (m_editorOutputSplit < 0.15f) m_editorOutputSplit = 0.15f;
        if (m_editorOutputSplit > 0.90f) m_editorOutputSplit = 0.90f;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    // Output console
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.051f, 0.051f, 0.071f, 1.0f));
    ImGui::BeginChild("##OutputBg", ImVec2(-1, -1), true);

    bool outputAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f;
    auto entries = Logging::Logger::GetInstance().GetEntries(50);
    for (const auto& entry : entries) {
        ImGui::TextColored(LogLevelColor(entry.level), "[%s] %s",
            Logging::LogLevelToString(entry.level),
            entry.message.c_str());
    }
    if (outputAtBottom)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::EndChild(); // LeftCol

    // ---- Vertical splitter (left / script panel) ----
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.145f, 0.149f, 0.192f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::AccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Accent);
    ImGui::Button("##vsplit", ImVec2(splitterThick, contentH));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        float delta = ImGui::GetIO().MouseDelta.x;
        m_scriptPanelWidth -= delta;
        if (m_scriptPanelWidth < minPanelW) m_scriptPanelWidth = minPanelW;
        if (m_scriptPanelWidth > avail.x - minPanelW - splitterThick)
            m_scriptPanelWidth = avail.x - minPanelW - splitterThick;
    }
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    ImGui::SameLine();

    // ---- Right column: Script panel ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.063f, 0.086f, 1.0f));
    ImGui::BeginChild("##ScriptPanel", ImVec2(m_scriptPanelWidth, contentH), true);

    // Search bar
    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##scriptSearch", "Search...", m_scriptSearchFilter,
                             sizeof(m_scriptSearchFilter));
    ImGui::PopItemWidth();

    ImGui::Dummy(ImVec2(0, 2));

    if (AccentButton("+ New Script", ImVec2(-1, 24))) {
        m_popupTargetTab = -1;
        m_showNamePopup = true;
        std::memset(m_nameBuffer, 0, sizeof(m_nameBuffer));
    }

    ImGui::Dummy(ImVec2(0, 4));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));

    // Filtered script list
    std::string filterLower(m_scriptSearchFilter);
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    for (int i = 0; i < (int)m_scriptFiles.size(); i++) {
        std::string nameLower = m_scriptFiles[i];
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);

        if (!filterLower.empty() && nameLower.find(filterLower) == std::string::npos)
            continue;

        bool isOpen = (FindTabByPath("scripts/" + m_scriptFiles[i]) >= 0);

        if (isOpen)
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Accent);

        if (ImGui::Selectable(m_scriptFiles[i].c_str(), isOpen)) {
            OpenScriptInTab(m_scriptFiles[i]);
        }

        if (isOpen)
            ImGui::PopStyleColor();

        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            m_contextMenuScript = i;
            ImGui::OpenPopup("##ScriptCtx");
        }
    }

    if (ImGui::BeginPopup("##ScriptCtx")) {
        if (m_contextMenuScript >= 0 && m_contextMenuScript < (int)m_scriptFiles.size()) {
            if (ImGui::MenuItem("Open")) {
                OpenScriptInTab(m_scriptFiles[m_contextMenuScript]);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Rename")) {
                m_popupTargetScript = m_contextMenuScript;
                m_showRenamePopup = true;
                std::strncpy(m_nameBuffer, m_scriptFiles[m_contextMenuScript].c_str(),
                             sizeof(m_nameBuffer) - 1);
            }
            if (ImGui::MenuItem("Delete")) {
                m_popupTargetScript = m_contextMenuScript;
                m_showDeleteConfirm = true;
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- Bottom toolbar ----
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + toolbarPadX);

    if (GreenButton("Execute", ImVec2(90, 32))) {
        ExecuteLuaInput();
    }

    ImGui::SameLine();
    if (ImGui::Button("Open", ImVec2(70, 32))) {
        std::string path = ShowOpenFileDialog();
        if (!path.empty()) {
            namespace fs = std::filesystem;
            std::string name = fs::path(path).filename().string();
            int existing = FindTabByPath(path);
            if (existing >= 0) {
                m_activeEditorTab = existing;
            } else {
                EditorTab tab(name, path);
                std::ifstream f(path);
                if (f.is_open()) {
                    std::string content((std::istreambuf_iterator<char>(f)),
                                        std::istreambuf_iterator<char>());
                    size_t len = (std::min)(content.size(), tab.buffer.size() - 1);
                    std::memcpy(tab.buffer.data(), content.c_str(), len);
                }
                m_editorTabs.push_back(std::move(tab));
                m_activeEditorTab = (int)m_editorTabs.size() - 1;
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(70, 32))) {
        if (!m_editorTabs.empty())
            SaveTab(m_activeEditorTab);
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(70, 32))) {
        if (!m_editorTabs.empty() && m_activeEditorTab < (int)m_editorTabs.size()) {
            std::memset(m_editorTabs[m_activeEditorTab].buffer.data(), 0,
                        m_editorTabs[m_activeEditorTab].buffer.size());
            m_editorTabs[m_activeEditorTab].isDirty = true;
        }
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##out")) {
        auto copyEntries = Logging::Logger::GetInstance().GetEntries(50);
        std::string all;
        for (const auto& e : copyEntries) {
            all += "[";
            all += Logging::LogLevelToString(e.level);
            all += "] ";
            all += e.message;
            all += "\n";
        }
        ImGui::SetClipboardText(all.c_str());
    }

    // Right-aligned: attach/detach + status
    auto& engine = Engine::GetInstance();
    float rightEnd = avail.x - toolbarPadX;
    float rightStart = rightEnd - 210.0f;
    if (rightStart > ImGui::GetCursorPosX() + 20.0f)
        ImGui::SameLine(rightStart);
    else
        ImGui::SameLine();

    if (engine.IsAttached()) {
        if (DangerButton("Detach", ImVec2(80, 32))) {
            engine.Detach();
            m_objectsNeedRefresh = true;
        }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::SameLine();
        ImGui::TextColored(Theme::Success, "PID:%lu", engine.GetPid());

        // Live privilege tier (cached 1s to avoid per-frame RPM)
        ImGui::SameLine();
        static int cachedLevel = 0;
        static auto lastCheck = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheck).count() > 1000) {
            cachedLevel = Privilege::GetPrivilegeLevel();
            lastCheck = now;
        }
        ImVec4 lvColor = (cachedLevel >= 7) ? Theme::Success
                       : (cachedLevel >= 4) ? Theme::Warning : Theme::Error;
        ImGui::TextColored(lvColor, "Lv:%d", cachedLevel);
    } else {
        if (AccentButton("Attach", ImVec2(80, 32))) {
            engine.AttachToProcess("test.exe");
            if (engine.IsAttached()) m_objectsNeedRefresh = true;
        }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::SameLine();
        ImGui::TextColored(Theme::Dim, "Not attached");
    }
}

// ============================================================
//  Injector Tab — Process Management + Privilege
// ============================================================

void GUI::RenderInjectorTab() {
    auto& engine = Engine::GetInstance();

    Section("Process Status");

    if (engine.IsAttached()) {
        ImGui::TextColored(Theme::Success, "ATTACHED");
        ImGui::SameLine();
        ImGui::TextColored(Theme::Dim, " — PID: %lu  |  Module: 0x%llX",
            engine.GetPid(), engine.GetModuleBase());

        ImGui::Dummy(ImVec2(0, 6));

        if (DangerButton("Detach", ImVec2(100, 30))) {
            engine.Detach();
            m_objectsNeedRefresh = true;
        }
    } else {
        ImGui::TextColored(Theme::Error, "NOT ATTACHED");

        ImGui::Dummy(ImVec2(0, 6));

        static char processInput[256] = "test.exe";
        ImGui::PushItemWidth(220);
        ImGui::InputText("##procName", processInput, sizeof(processInput));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (AccentButton("Attach", ImVec2(90, 0))) {
            try {
                DWORD pid = std::stoul(processInput);
                engine.AttachToProcess(pid);
            } catch (...) {
                engine.AttachToProcess(processInput);
            }
            if (engine.IsAttached()) m_objectsNeedRefresh = true;
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(name or PID)");
    }

    Section("Bootstrap (Educational Demo)");

    static char dllPath[512] = {};
    ImGui::PushItemWidth(-100);
    ImGui::InputText("##dllPath", dllPath, sizeof(dllPath));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Load DLL", ImVec2(90, 0))) {
        if (engine.IsAttached() && dllPath[0] != '\0') {
            Bootstrap::LoadIntoProcess(dllPath);
        }
    }

    ImGui::TextDisabled("Uses CreateRemoteThread + LoadLibraryA");

    Section("Script Identity Elevation");

    if (engine.IsAttached()) {
        Privilege::ContextInfo ctx;
        bool resolved = Privilege::ResolveContext(ctx);

        if (resolved) {
            ImGui::Text("DataModel:      0x%llX", (unsigned long long)ctx.dataModel);
            ImGui::SameLine();
            ImGui::TextColored(Theme::Dim, "(%s)", ctx.resolutionPath.c_str());

            ImGui::Text("ScriptContext:  0x%llX", (unsigned long long)ctx.scriptContext);

            ImVec4 levelColor = (ctx.currentLevel >= 7) ? Theme::Success
                                 : (ctx.currentLevel >= 4) ? Theme::Warning
                                 : Theme::Error;
            ImGui::Text("Current Level:");
            ImGui::SameLine();
            ImGui::TextColored(levelColor, "%d", ctx.currentLevel);
            ImGui::SameLine();
            ImGui::TextDisabled("(target: 10)");

            ImGui::Text("Bypass: %s   Detour: %s",
                ctx.requireBypass ? "ON" : "OFF",
                ctx.detourInstalled ? "INSTALLED" : "off");

            ImGui::Dummy(ImVec2(0, 6));

            ImGui::PushItemWidth(300);
            ImGui::SliderInt("##targetLevel", &m_privilegeTargetLevel, 1, 10, "Target Level: %d");
            ImGui::PopItemWidth();

            ImGui::Dummy(ImVec2(0, 6));

            if (AccentButton("Elevate Now", ImVec2(120, 30))) {
                try {
                    int result = Privilege::ElevateWithRetry(m_privilegeTargetLevel, false, 3, 500);
                    if (result >= m_privilegeTargetLevel)
                        LOG_INFO("[GUI] Elevation successful - level %d confirmed", result);
                    else if (result > 0)
                        LOG_WARN("[GUI] Elevation partial - level %d (target was %d)", result, m_privilegeTargetLevel);
                    else
                        LOG_ERROR("[GUI] Elevation failed - level unchanged");
                } catch (const std::exception& e) {
                    LOG_ERROR("[GUI] Elevation exception: %s", e.what());
                } catch (...) {
                    LOG_ERROR("[GUI] Elevation unknown exception");
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Install Detour", ImVec2(120, 30))) {
                try {
                    Privilege::InstallIdentityCheckDetour(m_privilegeTargetLevel);
                } catch (...) {
                    LOG_ERROR("[GUI] Detour installation failed");
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Remove Detour", ImVec2(120, 30))) {
                try {
                    Privilege::RemoveIdentityCheckDetour();
                } catch (...) {
                    LOG_ERROR("[GUI] Detour removal failed");
                }
            }

            ImGui::Dummy(ImVec2(0, 6));
            ImGui::Checkbox("Auto-elevate on attach", &m_privilegeAutoElevate);
            ImGui::Checkbox("Bypass security checks", &m_privilegeBypassChecks);

            ImGui::Dummy(ImVec2(0, 6));
            if (ImGui::Button("Run Diagnostics", ImVec2(140, 30))) {
                Privilege::RunDiagnostics();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Dumps chain data to Logger tab");
        } else {
            ImGui::TextColored(Theme::Warning,
                "ScriptContext not resolved: %s", ctx.lastError.c_str());
            ImGui::TextDisabled("Target may not be initialized or offset chain mismatch.");

            ImGui::Dummy(ImVec2(0, 6));
            if (AccentButton("Retry Elevation", ImVec2(140, 30))) {
                try {
                    int result = Privilege::ElevateWithRetry(m_privilegeTargetLevel, false, 5, 1000);
                    if (result >= m_privilegeTargetLevel)
                        LOG_INFO("[GUI] Elevation successful after retry - level %d", result);
                    else
                        LOG_ERROR("[GUI] Elevation failed after retries - level %d", result);
                } catch (const std::exception& e) {
                    LOG_ERROR("[GUI] Elevation exception: %s", e.what());
                } catch (...) {
                    LOG_ERROR("[GUI] Elevation unknown exception");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Run Diagnostics", ImVec2(140, 30))) {
                Privilege::RunDiagnostics();
            }
        }
    } else {
        ImGui::TextDisabled("Attach to a process to configure privilege level.");
    }
}

// ============================================================
//  Logger Tab — Filtered Event Log (scroll fixed)
// ============================================================

void GUI::RenderErrorLoggerTab() {
    static bool showTrace = false;
    static bool showDebug = true;
    static bool showInfo = true;
    static bool showWarn = true;
    static bool showError = true;
    static bool showFatal = true;

    ImGui::Dummy(ImVec2(0, 4));

    auto FilterToggle = [](const char* label, bool* state, ImVec4 color) {
        if (*state) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.40f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(color.x, color.y, color.z, 0.50f));
            ImGui::PushStyleColor(ImGuiCol_Text, color);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.14f, 0.18f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.08f, 0.11f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim);
        }
        if (ImGui::Button(label, ImVec2(70, 26)))
            *state = !*state;
        ImGui::PopStyleColor(4);
    };

    FilterToggle("Trace", &showTrace, Theme::Dim);
    ImGui::SameLine();
    FilterToggle("Debug", &showDebug, Theme::Info);
    ImGui::SameLine();
    FilterToggle("Info",  &showInfo,  ImVec4(0.78f, 0.79f, 0.82f, 1.0f));
    ImGui::SameLine();
    FilterToggle("Warn",  &showWarn,  Theme::Warning);
    ImGui::SameLine();
    FilterToggle("Error", &showError, Theme::Error);
    ImGui::SameLine();
    FilterToggle("Fatal", &showFatal, Theme::Fatal);

    ImGui::SameLine(0, 20);
    if (ImGui::Button("Clear Log", ImVec2(80, 26))) {
        Logging::Logger::GetInstance().ClearEntries();
    }

    ImGui::SameLine(0, 10);
    if (ImGui::Button("Copy Log", ImVec2(80, 26))) {
        auto copyEntries = Logging::Logger::GetInstance().GetEntries(500);
        std::string all;
        for (const auto& e : copyEntries) {
            auto ctt = std::chrono::system_clock::to_time_t(e.timestamp);
            auto cms = std::chrono::duration_cast<std::chrono::milliseconds>(
                e.timestamp.time_since_epoch()) % 1000;
            char ctb[16];
            std::tm ctm{};
            localtime_s(&ctm, &ctt);
            strftime(ctb, sizeof(ctb), "%H:%M:%S", &ctm);

            char line[2048];
            snprintf(line, sizeof(line), "[%s.%03lld] [%-5s] %s\n",
                ctb, (long long)cms.count(),
                Logging::LogLevelToString(e.level),
                e.message.c_str());
            all += line;
        }
        ImGui::SetClipboardText(all.c_str());
    }

    ImGui::Dummy(ImVec2(0, 4));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.051f, 0.051f, 0.071f, 1.0f));
    ImGui::BeginChild("##LogScroll", ImVec2(0, 0), true);

    bool logAtBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 10.0f;

    auto allEntries = Logging::Logger::GetInstance().GetEntries(500);
    for (const auto& entry : allEntries) {
        bool show = false;
        switch (entry.level) {
            case Logging::LogLevel::Trace: show = showTrace; break;
            case Logging::LogLevel::Debug: show = showDebug; break;
            case Logging::LogLevel::Info:  show = showInfo;  break;
            case Logging::LogLevel::Warn:  show = showWarn;  break;
            case Logging::LogLevel::Error: show = showError; break;
            case Logging::LogLevel::Fatal: show = showFatal; break;
        }
        if (!show) continue;

        auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;
        char timeBuf[16];
        std::tm tm{};
        localtime_s(&tm, &tt);
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

        ImGui::TextColored(LogLevelColor(entry.level), "[%s.%03lld] [%-5s] %s",
            timeBuf, ms.count(),
            Logging::LogLevelToString(entry.level),
            entry.message.c_str());
    }

    if (logAtBottom)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================
//  Objects Tab — Object Tree Browser
// ============================================================

void GUI::RenderObjectsTab() {
    auto& engine = Engine::GetInstance();

    if (!engine.IsAttached()) {
        ImGui::Dummy(ImVec2(0, 40));
        ImGui::TextColored(Theme::Dim, "  Attach to a process to browse objects.");
        return;
    }

    if (AccentButton("Scan Objects") || m_objectsNeedRefresh) {
        m_objectsNeedRefresh = false;
        m_cachedObjects.clear();
        m_cachedRemotes.clear();

        try {
            uintptr_t base = engine.GetModuleBase();
            if (base != 0) {
                uintptr_t fakeDM = Memory::Read<uintptr_t>(base + offsets::FakeDataModelPointer);
                if (fakeDM != 0) {
                    uintptr_t realDM = Memory::Read<uintptr_t>(fakeDM + offsets::FakeDataModelToDataModel);
                    if (realDM != 0) {
                        uintptr_t workspace = Memory::Read<uintptr_t>(realDM + offsets::Workspace);
                        if (workspace != 0) {
                            uintptr_t childStart = Memory::Read<uintptr_t>(workspace + offsets::Children);
                            uintptr_t childEnd   = Memory::Read<uintptr_t>(workspace + offsets::Children + 0x8);
                            const size_t kMaxScan = 500;
                            size_t scanned = 0;

                            if (childStart != 0 && childEnd > childStart) {
                                for (uintptr_t cur = childStart; cur < childEnd && scanned < kMaxScan;
                                     cur += sizeof(uintptr_t), ++scanned) {
                                    uintptr_t child = Memory::Read<uintptr_t>(cur);
                                    if (child == 0) continue;

                                    ObjectEntry entry{};
                                    entry.address = child;
                                    entry.name = Memory::ReadString(child + offsets::Name, 128);

                                    uintptr_t cd = Memory::Read<uintptr_t>(child + offsets::ClassDescriptor);
                                    if (cd != 0) {
                                        uintptr_t cnPtr = Memory::Read<uintptr_t>(cd + offsets::ClassDescriptorToClassName);
                                        if (cnPtr != 0)
                                            entry.className = Memory::ReadString(cnPtr, 64);
                                    }

                                    if (!entry.name.empty()) {
                                        m_cachedObjects.push_back(entry);

                                        if (entry.className == "RemoteEvent" ||
                                            entry.className == "RemoteFunction") {
                                            RemoteEntry re{};
                                            re.name = entry.name;
                                            re.type = entry.className;
                                            re.address = entry.address;
                                            m_cachedRemotes.push_back(re);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } catch (...) {
            LOG_ERROR("[Objects] Failed to scan objects");
        }
    }

    ImGui::SameLine();
    ImGui::TextColored(Theme::Dim, "%zu objects, %zu communication",
        m_cachedObjects.size(), m_cachedRemotes.size());

    ImGui::Dummy(ImVec2(0, 4));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.055f, 0.078f, 1.0f));
    ImGui::BeginChild("##ObjectTree", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() * 6)), true);

    if (m_cachedObjects.empty())
        ImGui::TextDisabled("No objects found. Click 'Scan Objects' to populate.");

    for (const auto& obj : m_cachedObjects) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        bool isRemote = (obj.className == "RemoteEvent" || obj.className == "RemoteFunction");

        if (isRemote) ImGui::PushStyleColor(ImGuiCol_Text, Theme::Accent);

        ImGui::TreeNodeEx((void*)obj.address, flags,
            "%s [%s]", obj.name.c_str(), obj.className.c_str());

        if (isRemote) ImGui::PopStyleColor();

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Address: 0x%llX", obj.address);
            ImGui::Text("Class:   %s", obj.className.c_str());
            ImGui::EndTooltip();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    Section("Communication Objects");

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.055f, 0.055f, 0.078f, 1.0f));
    ImGui::BeginChild("##RemoteList", ImVec2(0, 0), true);

    if (m_cachedRemotes.empty())
        ImGui::TextDisabled("No communication objects detected.");

    for (const auto& re : m_cachedRemotes) {
        ImGui::BulletText("[%s] %s @ 0x%llX",
            re.type.c_str(), re.name.c_str(), re.address);

        ImGui::SameLine();
        ImGui::PushID(static_cast<int>(re.address));
        if (ImGui::SmallButton("Log")) {
            GuiWidget w{};
            w.type = GuiWidget::Type::Log;
            w.text = "Logged " + re.type + ": " + re.name;
            LuaBridge::GetInstance().QueueWidget(w);
        }
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// ============================================================
//  Popup Modals — Save / Name / Delete / Rename
// ============================================================

void GUI::RenderPopups() {
    // ---- Save Confirm ----
    if (m_showSaveConfirm) {
        ImGui::OpenPopup("Save Changes?##modal");
        m_showSaveConfirm = false;
    }

    bool saveOpen = true;
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Save Changes?##modal", &saveOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_popupTargetTab >= 0 && m_popupTargetTab < (int)m_editorTabs.size())
            ImGui::Text("Save changes to \"%s\"?", m_editorTabs[m_popupTargetTab].name.c_str());
        else
            ImGui::Text("You have unsaved changes.\nSave before closing?");

        ImGui::Dummy(ImVec2(0, 10));

        if (GreenButton("Yes", ImVec2(90, 30))) {
            if (m_popupTargetTab >= 0 && m_popupTargetTab < (int)m_editorTabs.size()) {
                auto& tab = m_editorTabs[m_popupTargetTab];
                if (tab.filePath.empty()) {
                    m_showNamePopup = true;
                    std::memset(m_nameBuffer, 0, sizeof(m_nameBuffer));
                } else {
                    std::ofstream f(tab.filePath);
                    if (f.is_open()) {
                        f << tab.buffer.data();
                        tab.isDirty = false;
                    }
                    ForceCloseTab(m_popupTargetTab);
                    if (m_closingApp && !HasUnsavedTabs())
                        m_running = false;
                }
            } else if (m_closingApp) {
                bool needsName = false;
                for (auto& tab : m_editorTabs) {
                    if (tab.isDirty) {
                        if (tab.filePath.empty()) {
                            needsName = true;
                        } else {
                            std::ofstream f(tab.filePath);
                            if (f.is_open()) {
                                f << tab.buffer.data();
                                tab.isDirty = false;
                            }
                        }
                    }
                }
                if (!needsName) {
                    m_running = false;
                } else {
                    for (int i = 0; i < (int)m_editorTabs.size(); i++) {
                        if (m_editorTabs[i].isDirty && m_editorTabs[i].filePath.empty()) {
                            m_popupTargetTab = i;
                            m_showNamePopup = true;
                            std::memset(m_nameBuffer, 0, sizeof(m_nameBuffer));
                            break;
                        }
                    }
                }
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (DangerButton("No", ImVec2(90, 30))) {
            if (m_popupTargetTab >= 0)
                ForceCloseTab(m_popupTargetTab);
            if (m_closingApp)
                m_running = false;
            m_closingApp = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 30))) {
            m_closingApp = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    if (!saveOpen)
        m_closingApp = false;

    // ---- Name Script ----
    if (m_showNamePopup) {
        ImGui::OpenPopup("Name Your Script##modal");
        m_showNamePopup = false;
    }

    bool nameOpen = true;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Name Your Script##modal", &nameOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Enter a name for the script:");
        ImGui::Dummy(ImVec2(0, 4));

        ImGui::PushItemWidth(-1);
        bool enter = ImGui::InputText("##scriptNameInput", m_nameBuffer, sizeof(m_nameBuffer),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 8));

        if (GreenButton("Save", ImVec2(90, 30)) || enter) {
            std::string name(m_nameBuffer);
            if (!name.empty()) {
                if (name.size() < 4 || name.substr(name.size() - 4) != ".lua")
                    name += ".lua";

                std::string path = "scripts/" + name;
                std::filesystem::create_directories("scripts");

                if (m_popupTargetTab >= 0 && m_popupTargetTab < (int)m_editorTabs.size()) {
                    auto& tab = m_editorTabs[m_popupTargetTab];
                    std::ofstream f(path);
                    if (f.is_open()) {
                        f << tab.buffer.data();
                        tab.name = name;
                        tab.filePath = path;
                        tab.isDirty = false;
                        LOG_INFO("[GUI] Saved: %s", path.c_str());
                    }

                    if (m_closingApp && !HasUnsavedTabs())
                        m_running = false;
                } else {
                    std::ofstream f(path);
                    if (f.is_open()) {
                        f << "-- " << name << "\n";
                        LOG_INFO("[GUI] Created: %s", path.c_str());
                    }
                    RefreshScriptList();
                    OpenScriptInTab(name);
                }

                RefreshScriptList();
                ImGui::CloseCurrentPopup();
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 30))) {
            m_closingApp = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // ---- Delete Confirm ----
    if (m_showDeleteConfirm) {
        ImGui::OpenPopup("Delete Script?##modal");
        m_showDeleteConfirm = false;
    }

    bool deleteOpen = true;
    ImGui::SetNextWindowSize(ImVec2(340, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Delete Script?##modal", &deleteOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_popupTargetScript >= 0 && m_popupTargetScript < (int)m_scriptFiles.size()) {
            ImGui::Text("Delete \"%s\"?", m_scriptFiles[m_popupTargetScript].c_str());
            ImGui::TextColored(Theme::Dim, "This cannot be undone.");

            ImGui::Dummy(ImVec2(0, 8));

            if (DangerButton("Delete", ImVec2(90, 30))) {
                std::string path = "scripts/" + m_scriptFiles[m_popupTargetScript];
                int tabIdx = FindTabByPath(path);
                if (tabIdx >= 0)
                    ForceCloseTab(tabIdx);

                try {
                    std::filesystem::remove(path);
                    LOG_INFO("[GUI] Deleted: %s", path.c_str());
                } catch (...) {
                    LOG_ERROR("[GUI] Failed to delete: %s", path.c_str());
                }

                RefreshScriptList();
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90, 30))) {
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // ---- Rename Script ----
    if (m_showRenamePopup) {
        ImGui::OpenPopup("Rename Script##modal");
        m_showRenamePopup = false;
    }

    bool renameOpen = true;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Rename Script##modal", &renameOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("New name:");
        ImGui::Dummy(ImVec2(0, 4));

        ImGui::PushItemWidth(-1);
        bool enter = ImGui::InputText("##renameInput", m_nameBuffer, sizeof(m_nameBuffer),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        ImGui::Dummy(ImVec2(0, 8));

        if (AccentButton("Rename", ImVec2(90, 30)) || enter) {
            if (m_popupTargetScript >= 0 && m_popupTargetScript < (int)m_scriptFiles.size()) {
                std::string newName(m_nameBuffer);
                if (!newName.empty()) {
                    if (newName.size() < 4 || newName.substr(newName.size() - 4) != ".lua")
                        newName += ".lua";

                    std::string oldPath = "scripts/" + m_scriptFiles[m_popupTargetScript];
                    std::string newPath = "scripts/" + newName;

                    try {
                        std::filesystem::rename(oldPath, newPath);

                        int tabIdx = FindTabByPath(oldPath);
                        if (tabIdx >= 0) {
                            m_editorTabs[tabIdx].name = newName;
                            m_editorTabs[tabIdx].filePath = newPath;
                        }

                        LOG_INFO("[GUI] Renamed: %s -> %s", oldPath.c_str(), newPath.c_str());
                    } catch (...) {
                        LOG_ERROR("[GUI] Failed to rename script");
                    }

                    RefreshScriptList();
                }
            }
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90, 30))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// ============================================================
//  Lua Execution
// ============================================================

void GUI::ExecuteLuaInput() {
    if (m_editorTabs.empty() || m_activeEditorTab >= (int)m_editorTabs.size())
        return;

    auto& tab = m_editorTabs[m_activeEditorTab];
    std::string code(tab.buffer.data());
    if (code.empty()) return;

    LOG_INFO("> %s", code.c_str());
    m_consoleHistory.push_back(code);
    m_historyPos = -1;

    auto& bridge = LuaBridge::GetInstance();
    if (bridge.ExecuteString(code))
        LOG_INFO("[Lua] Executed successfully");
    else
        LOG_ERROR("[Lua] Execution failed");
}

// ============================================================
//  Widget Queue Processing
// ============================================================

void GUI::ProcessWidgetQueue() {
    auto widgets = LuaBridge::GetInstance().FlushWidgetQueue();
    if (widgets.empty()) return;

    for (const auto& w : widgets) {
        switch (w.type) {
            case GuiWidget::Type::Log:
                LOG_INFO("%s", w.text.c_str());
                break;
            case GuiWidget::Type::Text:
                LOG_DEBUG("[TEXT] %s: %s", w.tabName.c_str(), w.text.c_str());
                break;
            case GuiWidget::Type::Slider:
                LOG_DEBUG("[Widget] Slider '%s/%s' = %.3f",
                    w.tabName.c_str(), w.label.c_str(), w.values[0]);
                break;
            case GuiWidget::Type::Checkbox:
                LOG_DEBUG("[Widget] Checkbox '%s/%s' = %s",
                    w.tabName.c_str(), w.label.c_str(),
                    w.values[0] > 0.5f ? "on" : "off");
                break;
            default:
                break;
        }
    }
}

// ============================================================
//  Script List
// ============================================================

void GUI::RefreshScriptList() {
    m_scriptFiles.clear();
    try {
        namespace fs = std::filesystem;
        if (fs::exists("scripts")) {
            for (const auto& entry : fs::directory_iterator("scripts")) {
                if (entry.is_regular_file() && entry.path().extension() == ".lua") {
                    m_scriptFiles.push_back(entry.path().filename().string());
                }
            }
            std::sort(m_scriptFiles.begin(), m_scriptFiles.end());
        }
    } catch (...) {
        LOG_WARN("[GUI] Failed to scan scripts/ directory");
    }
}

// ============================================================
//  Editor Tab Helpers
// ============================================================

int GUI::FindTabByPath(const std::string& path) {
    for (int i = 0; i < (int)m_editorTabs.size(); i++) {
        if (m_editorTabs[i].filePath == path)
            return i;
    }
    return -1;
}

void GUI::OpenScriptInTab(const std::string& filename) {
    std::string path = "scripts/" + filename;

    int existing = FindTabByPath(path);
    if (existing >= 0) {
        m_activeEditorTab = existing;
        return;
    }

    EditorTab tab(filename, path);
    std::ifstream f(path);
    if (f.is_open()) {
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        size_t len = (std::min)(content.size(), tab.buffer.size() - 1);
        std::memcpy(tab.buffer.data(), content.c_str(), len);
    }

    m_editorTabs.push_back(std::move(tab));
    m_activeEditorTab = (int)m_editorTabs.size() - 1;
}

void GUI::RequestCloseTab(int index) {
    if (index < 0 || index >= (int)m_editorTabs.size()) return;

    if (m_editorTabs[index].isDirty) {
        m_popupTargetTab = index;
        m_showSaveConfirm = true;
    } else {
        ForceCloseTab(index);
    }
}

void GUI::ForceCloseTab(int index) {
    if (index < 0 || index >= (int)m_editorTabs.size()) return;

    m_editorTabs.erase(m_editorTabs.begin() + index);

    if (m_activeEditorTab >= (int)m_editorTabs.size())
        m_activeEditorTab = (int)m_editorTabs.size() - 1;
    if (m_activeEditorTab < 0) m_activeEditorTab = 0;

    if (m_editorTabs.empty())
        CreateNewTab();
}

void GUI::SaveTab(int index) {
    if (index < 0 || index >= (int)m_editorTabs.size()) return;

    auto& tab = m_editorTabs[index];

    if (tab.filePath.empty()) {
        m_popupTargetTab = index;
        m_showNamePopup = true;
        std::memset(m_nameBuffer, 0, sizeof(m_nameBuffer));
        return;
    }

    std::ofstream f(tab.filePath);
    if (f.is_open()) {
        f << tab.buffer.data();
        tab.isDirty = false;
        LOG_INFO("[GUI] Saved: %s", tab.filePath.c_str());
    } else {
        LOG_ERROR("[GUI] Failed to save: %s", tab.filePath.c_str());
    }
}

bool GUI::HasUnsavedTabs() {
    for (const auto& tab : m_editorTabs) {
        if (tab.isDirty) return true;
    }
    return false;
}

void GUI::CreateNewTab() {
    std::string name = "Untitled-" + std::to_string(m_untitledCounter++);
    m_editorTabs.emplace_back(name);
    m_activeEditorTab = (int)m_editorTabs.size() - 1;
}

// ============================================================
//  Config Save / Load
// ============================================================

void GUI::SaveConfig() {
    try {
        nlohmann::json config;

        config["gui"]["visible"] = m_visible;
        config["gui"]["toggle_hotkey"] = m_toggleHotkey;

        config["privilege"]["target_level"] = m_privilegeTargetLevel;
        config["privilege"]["auto_elevate_on_attach"] = m_privilegeAutoElevate;
        config["privilege"]["bypass_security_checks"] = m_privilegeBypassChecks;

        std::ofstream file("config/user_config.json");
        if (file.is_open()) {
            file << config.dump(2);
            file.close();
            LOG_INFO("[OK] Config saved to config/user_config.json");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[ERROR] Failed to save config: %s", e.what());
    }
}

void GUI::LoadConfig() {
    try {
        std::ifstream file("config/user_config.json");
        if (!file.is_open()) return;

        nlohmann::json config;
        file >> config;
        file.close();

        if (config.contains("gui")) {
            if (config["gui"].contains("visible"))
                m_visible = config["gui"]["visible"];
            if (config["gui"].contains("toggle_hotkey"))
                m_toggleHotkey = config["gui"]["toggle_hotkey"];
        }

        if (config.contains("privilege")) {
            if (config["privilege"].contains("target_level"))
                m_privilegeTargetLevel = config["privilege"]["target_level"];
            if (config["privilege"].contains("auto_elevate_on_attach"))
                m_privilegeAutoElevate = config["privilege"]["auto_elevate_on_attach"];
            if (config["privilege"].contains("bypass_security_checks"))
                m_privilegeBypassChecks = config["privilege"]["bypass_security_checks"];
        }

        LOG_INFO("[OK] Config loaded from config/user_config.json");
    } catch (const std::exception& e) {
        LOG_WARN("[WARN] Failed to load config: %s", e.what());
    }
}
