/**
 * GUI.cpp — Dear ImGui / GLFW Control Panel Implementation
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#include "GUI.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstring>

// ImGui headers
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

// GLFW
#include <GLFW/glfw3.h>

// Project headers
#include "../Lua/LuaBridge.h"
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Bootstrap.h"
#include "../Core/offsets.h"

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
    std::cerr << "[GUI] GLFW Error " << error << ": " << description << std::endl;
}

bool GUI::Initialize(const std::string& title, int width, int height) {
    // ---- GLFW Setup ----
    glfwSetErrorCallback(GlfwErrorCallback);

    if (!glfwInit()) {
        std::cerr << "[GUI] Failed to initialize GLFW" << std::endl;
        return false;
    }

    // OpenGL 3.3 for broad compatibility
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) {
        std::cerr << "[GUI] Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_window);
    glfwSwapInterval(1); // V-Sync for smooth rendering

    // ---- Dear ImGui Setup ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // Enable docking
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "config/imgui.ini";  // Persist layout

    // Style: dark theme with slightly customized colors
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.12f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.28f, 0.40f, 0.55f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.35f, 0.50f, 0.70f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.20f, 0.32f, 0.50f, 0.80f);

    // Platform / Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(m_window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // Load config if exists
    LoadConfig();

    m_running = true;
    m_lastFrameTime = std::chrono::steady_clock::now();

    std::cout << "[GUI] Initialized — " << width << "x" << height
              << " (INSERT to toggle)" << std::endl;
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

    std::cout << "[GUI] Shut down" << std::endl;
}

// ============================================================
//  Main Loop
// ============================================================

void GUI::Run() {
    if (!m_window || !m_running) return;

    while (!glfwWindowShouldClose(m_window) && m_running) {
        glfwPollEvents();

        // ---- Delta time ----
        auto now = std::chrono::steady_clock::now();
        m_deltaTime = std::chrono::duration<float>(now - m_lastFrameTime).count();
        m_lastFrameTime = now;

        // ---- INSERT hotkey toggle ----
        if (m_toggleHotkey != 0) {
            bool pressed = glfwGetKey(m_window, m_toggleHotkey) == GLFW_PRESS;
            if (pressed && !m_hotkeyWasPressed) {
                m_visible = !m_visible;
                std::cout << "[GUI] " << (m_visible ? "Shown" : "Hidden") << std::endl;
            }
            m_hotkeyWasPressed = pressed;
        }

        // ---- Start ImGui frame ----
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (m_visible) {
            SetupDockingLayout();

            // ---- Consume widget queue from LuaBridge ----
            ProcessWidgetQueue();

            // ---- Menu bar ----
            RenderMenuBar();

            // ---- Tab panels ----
            RenderGeneralTab();
            RenderObjectsTab();
            RenderVisualsTab();
            RenderAutomationTab();
            RenderConsoleTab();
            RenderLogPanel();

            // ---- Demo / Metrics windows ----
            if (m_showDemoWindow) ImGui::ShowDemoWindow(&m_showDemoWindow);
            if (m_showMetrics)  ImGui::ShowMetricsWindow(&m_showMetrics);
        }

        // ---- Lua module lifecycle (runs even when GUI hidden) ----
        LuaBridge::GetInstance().OnFrame();

        // ---- Render ----
        ImGui::Render();

        int displayW, displayH;
        glfwGetFramebufferSize(m_window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.08f, 0.09f, 0.12f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(m_window);
    }

    // Window was closed — cleanup
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
//  Docking Layout
// ============================================================

void GUI::SetupDockingLayout() {
    // Create a full-window dockspace
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGuiWindowFlags dockspaceFlags =
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground;

    ImGui::Begin("UniversalHubDockSpace", nullptr, dockspaceFlags);
    ImGui::PopStyleVar(2);

    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    // On first frame, set up the default dock layout
    static bool firstFrame = true;
    if (firstFrame) {
        firstFrame = false;

        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->Size);

        // Split into left (main content) and right (log)
        ImGuiID dockRight;
        ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.75f,
                                                        nullptr, &dockRight);

        // Split left into top (General/Automation) and bottom (Console)
        ImGuiID dockBottom, dockTop;
        dockTop = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.45f,
                                              nullptr, &dockBottom);

        // Split top into General (left) and Automation (right)
        ImGuiID dockAutomation;
        ImGuiID dockGeneral = ImGui::DockBuilderSplitNode(dockTop, ImGuiDir_Left, 0.45f,
                                                           nullptr, &dockAutomation);

        // Split General area into General (top) and Visuals (bottom)
        ImGuiID dockVisuals;
        dockGeneral = ImGui::DockBuilderSplitNode(dockGeneral, ImGuiDir_Up, 0.50f,
                                                   nullptr, &dockVisuals);

        // Dock windows
        ImGui::DockBuilderDockWindow("General",      dockGeneral);
        ImGui::DockBuilderDockWindow("Visuals",      dockVisuals);
        ImGui::DockBuilderDockWindow("Automation",   dockAutomation);
        ImGui::DockBuilderDockWindow("Objects",      dockAutomation);
        ImGui::DockBuilderDockWindow("Console",      dockBottom);
        ImGui::DockBuilderDockWindow("Log",          dockRight);

        ImGui::DockBuilderFinish(dockspaceId);
    }

    ImGui::End(); // DockSpace window
}

// ============================================================
//  Menu Bar
// ============================================================

void GUI::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Config", "Ctrl+S")) {
                SaveConfig();
            }
            if (ImGui::MenuItem("Load Config", "Ctrl+L")) {
                LoadConfig();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                RequestStop();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Demo Window", nullptr, &m_showDemoWindow);
            ImGui::MenuItem("Metrics", nullptr, &m_showMetrics);
            ImGui::Separator();
            if (ImGui::MenuItem("Hide GUI", "INSERT")) {
                m_visible = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            ImGui::Text("Universal Hub v1.0.0");
            ImGui::Text("Educational Process Interaction Framework");
            ImGui::Separator();
            ImGui::Text("FOR EDUCATIONAL DEMONSTRATION ONLY");
            ImGui::EndMenu();
        }

        // FPS display on the right
        ImVec2 cursorPos = ImGui::GetCursorPos();
        ImGui::SameLine(ImGui::GetWindowWidth() - 120.0f);
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

        ImGui::EndMainMenuBar();
    }
}

// ============================================================
//  General Tab
// ============================================================

void GUI::RenderGeneralTab() {
    ImGui::Begin("General");

    auto& engine = Engine::GetInstance();

    // ---- Process Status ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Process Status");
    ImGui::Separator();

    if (engine.IsAttached()) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "  ATTACHED");
        ImGui::Text("  PID:        %lu", engine.GetPid());
        ImGui::Text("  Module:     0x%llX", engine.GetModuleBase());
        ImGui::Text("  Handle:     0x%p", engine.GetProcessHandle());

        if (ImGui::Button("Detach", ImVec2(120, 25))) {
            engine.Detach();
            m_objectsNeedRefresh = true;
        }
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  NOT ATTACHED");

        static char processInput[256] = "test.exe";
        ImGui::PushItemWidth(200);
        ImGui::InputText("##procName", processInput, sizeof(processInput));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button("Attach", ImVec2(80, 25))) {
            try {
                DWORD pid = std::stoul(processInput);
                engine.AttachToProcess(pid);
            } catch (...) {
                engine.AttachToProcess(processInput);
            }
            if (engine.IsAttached()) {
                m_objectsNeedRefresh = true;
            }
        }

        ImGui::SameLine();
        ImGui::TextDisabled("(name or PID)");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ---- Bootstrap ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Bootstrap (Educational Demo)");
    ImGui::Separator();

    static char dllPath[512] = {};
    ImGui::PushItemWidth(350);
    ImGui::InputText("##dllPath", dllPath, sizeof(dllPath));
    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Load DLL")) {
        if (engine.IsAttached() && dllPath[0] != '\0') {
            Bootstrap::LoadIntoProcess(dllPath);
        }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Uses CreateRemoteThread + LoadLibraryA");

    ImGui::End();
}

// ============================================================
//  Objects Tab
// ============================================================

void GUI::RenderObjectsTab() {
    ImGui::Begin("Objects");

    auto& engine = Engine::GetInstance();

    if (!engine.IsAttached()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Attach to a process to browse objects.");
        ImGui::End();
        return;
    }

    // ---- Refresh button ----
    if (ImGui::Button("Scan Objects") || m_objectsNeedRefresh) {
        m_objectsNeedRefresh = false;
        m_cachedObjects.clear();
        m_cachedRemotes.clear();

        try {
            uintptr_t base = engine.GetModuleBase();
            if (base != 0) {
                // Walk DataModel → Workspace → Children
                uintptr_t fakeDM = Memory::Read<uintptr_t>(base + offsets::FakeDataModelPointer);
                if (fakeDM != 0) {
                    uintptr_t realDM = Memory::Read<uintptr_t>(fakeDM + offsets::FakeDataModelToDataModel);
                    if (realDM != 0) {
                        uintptr_t workspace = Memory::Read<uintptr_t>(realDM + offsets::Workspace);
                        if (workspace != 0) {
                            // Enumerate children
                            uintptr_t child = Memory::Read<uintptr_t>(workspace + offsets::Children);
                            const int kMaxScan = 500;
                            for (int i = 0; i < kMaxScan && child != 0; ++i) {
                                ObjectEntry entry{};
                                entry.address = child;
                                entry.name = Memory::ReadString(child + offsets::Name, 128);

                                // Resolve class name
                                uintptr_t cd = Memory::Read<uintptr_t>(child + offsets::ClassDescriptor);
                                if (cd != 0) {
                                    uintptr_t cnPtr = Memory::Read<uintptr_t>(cd + offsets::ClassDescriptorToClassName);
                                    if (cnPtr != 0) {
                                        entry.className = Memory::ReadString(cnPtr, 64);
                                    }
                                }

                                if (!entry.name.empty()) {
                                    m_cachedObjects.push_back(entry);

                                    // Check if communication object
                                    if (entry.className == "RemoteEvent" ||
                                        entry.className == "RemoteFunction") {
                                        RemoteEntry re{};
                                        re.name = entry.name;
                                        re.type = entry.className;
                                        re.address = entry.address;
                                        m_cachedRemotes.push_back(re);
                                    }
                                }

                                child = Memory::Read<uintptr_t>(child + offsets::ChildrenEnd);
                            }
                        }
                    }
                }
            }
        } catch (...) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                               "Failed to scan objects.");
        }
    }

    ImGui::SameLine();
    ImGui::Text("Found: %zu objects, %zu communication objects",
                m_cachedObjects.size(), m_cachedRemotes.size());

    ImGui::Separator();

    // ---- Object tree ----
    ImGui::BeginChild("ObjectTree", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 5), true);

    if (m_cachedObjects.empty()) {
        ImGui::TextDisabled("No objects found. Click 'Scan Objects' to populate.");
    }

    for (const auto& obj : m_cachedObjects) {
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (obj.className == "RemoteEvent" || obj.className == "RemoteFunction") {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.3f, 1.0f));
        }

        bool open = ImGui::TreeNodeEx((void*)obj.address, flags,
                                       "%s [%s]", obj.name.c_str(), obj.className.c_str());

        if (obj.className == "RemoteEvent" || obj.className == "RemoteFunction") {
            ImGui::PopStyleColor();
        }

        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Address: 0x%llX", obj.address);
            ImGui::Text("Class:   %s", obj.className.c_str());
            ImGui::EndTooltip();
        }
    }

    ImGui::EndChild();

    // ---- Communication objects ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Communication Objects");
    ImGui::Separator();

    ImGui::BeginChild("RemoteList", ImVec2(0, 0), true);

    if (m_cachedRemotes.empty()) {
        ImGui::TextDisabled("No communication objects detected.");
    }

    for (const auto& re : m_cachedRemotes) {
        ImGui::BulletText("[%s] %s @ 0x%llX",
                          re.type.c_str(), re.name.c_str(), re.address);

        ImGui::SameLine();
        ImGui::PushID(re.address);
        if (ImGui::SmallButton("Log")) {
            // Queue a log widget showing this remote's info
            GuiWidget w{};
            w.type = GuiWidget::Type::Log;
            w.text = "Logged " + re.type + ": " + re.name +
                     " (0x" + std::to_string(re.address) + ")";
            LuaBridge::GetInstance().QueueWidget(w);
        }
        ImGui::PopID();
    }

    ImGui::EndChild();

    ImGui::End();
}

// ============================================================
//  Visuals Tab
// ============================================================

void GUI::RenderVisualsTab() {
    ImGui::Begin("Visuals");

    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Attach to a process to adjust visual parameters.");
        ImGui::End();
        return;
    }

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Object Highlight");
    ImGui::Separator();

    static bool highlightEnabled = false;
    static float highlightColor[3] = {0.0f, 1.0f, 0.0f};
    static float highlightRadius = 30.0f;

    if (ImGui::Checkbox("Enable Highlight", &highlightEnabled)) {
        try {
            // Write to the target process if a highlight field exists
            // (depends on the specific target's internals)
        } catch (...) {}
    }

    ImGui::ColorEdit3("Highlight Color", highlightColor);
    ImGui::SliderFloat("Radius", &highlightRadius, 1.0f, 100.0f);

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Display Settings");
    ImGui::Separator();

    static bool nameTagsEnabled = false;
    static float nameTagDistance = 100.0f;

    ImGui::Checkbox("Show Name Tags", &nameTagsEnabled);
    ImGui::SliderFloat("Name Tag Distance", &nameTagDistance, 10.0f, 500.0f);

    ImGui::End();
}

// ============================================================
//  Automation Tab
// ============================================================

void GUI::RenderAutomationTab() {
    ImGui::Begin("Automation");

    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "Attach to a process to adjust automation parameters.");
        ImGui::End();
        return;
    }

    // ---- Movement ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Movement");
    ImGui::Separator();

    static float walkSpeed = 16.0f;
    static float jumpPower = 50.0f;
    static float hipHeight = 0.0f;

    if (ImGui::SliderFloat("Walk Speed", &walkSpeed, 0.0f, 200.0f)) {
        try {
            // Write to local player's WalkSpeed offset
            // (requires finding the local player object first)
        } catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##ws")) walkSpeed = 16.0f;

    if (ImGui::SliderFloat("Jump Power", &jumpPower, 0.0f, 500.0f)) {
        try {} catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##jp")) jumpPower = 50.0f;

    if (ImGui::SliderFloat("Hip Height", &hipHeight, -5.0f, 20.0f)) {
        try {} catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##hh")) hipHeight = 0.0f;

    ImGui::Spacing();

    // ---- Camera ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Camera");
    ImGui::Separator();

    static float fov = 70.0f;
    if (ImGui::SliderFloat("Field of View", &fov, 1.0f, 120.0f)) {
        try {} catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##fov")) fov = 70.0f;

    ImGui::Spacing();

    // ---- Environment ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Environment");
    ImGui::Separator();

    static float gravity = 196.2f;
    static float brightness = 2.0f;
    static float clockTime = 14.0f;

    if (ImGui::SliderFloat("Gravity", &gravity, 0.0f, 500.0f)) {
        try {} catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##gv")) gravity = 196.2f;

    if (ImGui::SliderFloat("Brightness", &brightness, 0.0f, 10.0f)) {
        try {} catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##br")) brightness = 2.0f;

    if (ImGui::SliderFloat("Clock Time", &clockTime, 0.0f, 24.0f)) {
        try {} catch (...) {}
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset##ct")) clockTime = 14.0f;

    ImGui::End();
}

// ============================================================
//  Console Tab
// ============================================================

void GUI::RenderConsoleTab() {
    ImGui::Begin("Console");

    // ---- Lua input area ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Lua Script Editor");
    ImGui::Separator();

    ImGui::PushItemWidth(-1);
    bool executeLine = ImGui::InputTextMultiline("##luaInput", m_luaInputBuffer,
                                                  sizeof(m_luaInputBuffer),
                                                  ImVec2(-1, -60),
                                                  ImGuiInputTextFlags_CtrlEnterForNewLine |
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    // Execute on Enter (not Ctrl+Enter)
    if (executeLine) {
        ExecuteLuaInput();
    }

    // Execute button
    if (ImGui::Button("Execute (Enter)", ImVec2(140, 25))) {
        ExecuteLuaInput();
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear Output", ImVec2(120, 25))) {
        m_logLines.clear();
    }

    ImGui::SameLine();
    if (ImGui::Button("Load Script...", ImVec2(120, 25))) {
        // In a real app this would open a file dialog
        // For now, try loading scripts/universal_hub.lua
        LuaBridge::GetInstance().ExecuteFile("scripts/universal_hub.lua");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("Script: scripts/universal_hub.lua");

    ImGui::Separator();

    // ---- Console log output ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Output");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        m_logLines.clear();
    }

    ImGui::BeginChild("ConsoleOutput", ImVec2(0, 0), true);

    for (const auto& line : m_logLines) {
        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);

        // Color-code lines
        if (line.find("[ERROR]") != std::string::npos) {
            color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        } else if (line.find("[WARN]") != std::string::npos) {
            color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
        } else if (line.find("[OK]") != std::string::npos ||
                   line.find("success") != std::string::npos) {
            color = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        } else if (line.find(">") == 0) {
            color = ImVec4(0.5f, 0.7f, 1.0f, 1.0f);
        }

        ImGui::TextColored(color, "%s", line.c_str());
    }

    if (m_scrollToBottom) {
        ImGui::SetScrollHereY(1.0f);
        m_scrollToBottom = false;
    }

    ImGui::EndChild();

    ImGui::End();
}

void GUI::ExecuteLuaInput() {
    std::string code(m_luaInputBuffer);
    if (code.empty()) return;

    // Echo to console
    m_logLines.push_back("> " + code);
    m_consoleHistory.push_back(code);
    m_historyPos = -1;

    // Execute
    auto& bridge = LuaBridge::GetInstance();
    if (bridge.ExecuteString(code)) {
        m_logLines.push_back("[OK] Executed successfully");
    } else {
        m_logLines.push_back("[ERROR] Execution failed");
    }

    // Cleanup
    std::memset(m_luaInputBuffer, 0, sizeof(m_luaInputBuffer));
    m_scrollToBottom = true;
    m_reclaimFocus = true;
}

// ============================================================
//  Log Panel
// ============================================================

void GUI::RenderLogPanel() {
    ImGui::Begin("Log");

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Event Log");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        m_logLines.clear();
    }

    ImGui::Separator();

    ImGui::BeginChild("LogScroll", ImVec2(0, 0), true);

    // Show log lines with filtering
    static bool showErrors = true;
    static bool showWarnings = true;
    static bool showInfo = true;

    ImGui::Checkbox("Errors", &showErrors);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &showWarnings);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &showInfo);

    ImGui::Separator();

    for (const auto& line : m_logLines) {
        bool isError = line.find("[ERROR]") != std::string::npos;
        bool isWarning = line.find("[WARN]") != std::string::npos;

        if (isError && !showErrors) continue;
        if (isWarning && !showWarnings) continue;
        if (!isError && !isWarning && !showInfo) continue;

        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        if (isError) color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        else if (isWarning) color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);

        ImGui::TextColored(color, "%s", line.c_str());
    }

    ImGui::EndChild();

    ImGui::End();
}

// ============================================================
//  Widget Queue Processing
// ============================================================

void GUI::ProcessWidgetQueue() {
    auto widgets = LuaBridge::GetInstance().FlushWidgetQueue();
    if (widgets.empty()) return;

    auto& bridge = LuaBridge::GetInstance();

    for (const auto& w : widgets) {
        switch (w.type) {
            case GuiWidget::Type::Log: {
                m_logLines.push_back(w.text);
                m_scrollToBottom = true;
                break;
            }

            case GuiWidget::Type::Text: {
                // Text widgets are rendered inline in their tab
                // For now, add to log
                m_logLines.push_back("[TEXT] " + w.tabName + ": " + w.text);
                m_scrollToBottom = true;
                break;
            }

            case GuiWidget::Type::Slider: {
                // Cache slider state
                std::string key = w.tabName + "/" + w.label;
                auto& state = m_sliderStates[key];
                if (state.callbackRef == -1) {
                    state.value = w.values[0];
                    state.callbackRef = w.callbackRef;
                }
                break;
            }

            case GuiWidget::Type::Checkbox: {
                std::string key = w.tabName + "/" + w.label;
                auto& state = m_checkboxStates[key];
                if (state.callbackRef == -1) {
                    state.checked = w.values[0] > 0.5f;
                    state.callbackRef = w.callbackRef;
                }
                break;
            }

            case GuiWidget::Type::Button: {
                // Buttons are processed when rendered
                break;
            }

            case GuiWidget::Type::ColorPicker: {
                // Color pickers are cached similarly to sliders
                break;
            }

            default:
                break;
        }
    }
}

// ============================================================
//  Config Save / Load
// ============================================================

void GUI::SaveConfig() {
    try {
        nlohmann::json config;

        config["gui"]["visible"] = m_visible;
        config["gui"]["toggle_hotkey"] = m_toggleHotkey;

        std::ofstream file("config/user_config.json");
        if (file.is_open()) {
            file << config.dump(2);
            file.close();
            m_logLines.push_back("[OK] Config saved to config/user_config.json");
        }
    } catch (const std::exception& e) {
        m_logLines.push_back("[ERROR] Failed to save config: " + std::string(e.what()));
    }
    m_scrollToBottom = true;
}

void GUI::LoadConfig() {
    try {
        std::ifstream file("config/user_config.json");
        if (!file.is_open()) {
            // No saved config — use defaults
            return;
        }

        nlohmann::json config;
        file >> config;
        file.close();

        if (config.contains("gui")) {
            if (config["gui"].contains("visible"))
                m_visible = config["gui"]["visible"];
            if (config["gui"].contains("toggle_hotkey"))
                m_toggleHotkey = config["gui"]["toggle_hotkey"];
        }

        m_logLines.push_back("[OK] Config loaded from config/user_config.json");
    } catch (const std::exception& e) {
        m_logLines.push_back("[WARN] Failed to load config: " + std::string(e.what()));
    }
    m_scrollToBottom = true;
}
