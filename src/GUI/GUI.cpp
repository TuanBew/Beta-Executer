/**
 * GUI.cpp — Dear ImGui / GLFW Control Panel Implementation
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY
 */

#include "GUI.h"

#include "Logging/Logger.h"
#include "Core/PrivilegeElevation.h"
#include <fstream>
#include <algorithm>
#include <cstring>
#include <ctime>

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
    LOG_ERROR("[GUI] GLFW Error %d: %s", error, description);
}

bool GUI::Initialize(const std::string& title, int width, int height) {
    // ---- GLFW Setup ----
    glfwSetErrorCallback(GlfwErrorCallback);

    if (!glfwInit()) {
        LOG_ERROR("[GUI] Failed to initialize GLFW");
        return false;
    }

    // OpenGL 3.3 for broad compatibility
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
                LOG_INFO("[GUI] %s", m_visible ? "Shown" : "Hidden");
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
            RenderInjectorTab();
            RenderObjectsTab();
            RenderExecuterTab();
            RenderErrorLoggerTab();

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

        // Split into left (Executer + Objects) and right (Injector + Error Logger)
        ImGuiID dockRight;
        ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.70f,
                                                        nullptr, &dockRight);

        // Split left into Executer (top) and Objects (bottom)
        ImGuiID dockObjects;
        ImGuiID dockExecuter = ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.70f,
                                                            nullptr, &dockObjects);

        // Split right into Injector (top) and Error Logger (bottom)
        ImGuiID dockErrorLogger;
        ImGuiID dockInjector = ImGui::DockBuilderSplitNode(dockRight, ImGuiDir_Up, 0.55f,
                                                            nullptr, &dockErrorLogger);

        // Dock windows
        ImGui::DockBuilderDockWindow("Injector",      dockInjector);
        ImGui::DockBuilderDockWindow("Executer",      dockExecuter);
        ImGui::DockBuilderDockWindow("Error Logger",  dockErrorLogger);
        ImGui::DockBuilderDockWindow("Objects",       dockObjects);

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

void GUI::RenderInjectorTab() {
    ImGui::Begin("Injector");

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

    ImGui::Spacing();
    ImGui::Spacing();

    // ---- Privilege Elevation ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Script Identity Elevation");
    ImGui::Separator();

    if (engine.IsAttached()) {
        Privilege::ContextInfo ctx;
        bool resolved = Privilege::ResolveContext(ctx);

        if (resolved) {
            ImGui::Text("ScriptContext:   0x%llX", (unsigned long long)ctx.scriptContext);
            ImGui::Text("Current Level:   %d", ctx.currentLevel);

            ImVec4 levelColor = (ctx.currentLevel >= 7) ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                 : (ctx.currentLevel >= 4) ? ImVec4(1.0f, 0.8f, 0.3f, 1.0f)
                                 : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            ImGui::TextColored(levelColor, "                  (target: 7+)");

            ImGui::Text("Bypass:          %s", ctx.requireBypass ? "ON" : "OFF");
            ImGui::Text("Detour:          %s", ctx.detourInstalled ? "INSTALLED" : "off");

            ImGui::Spacing();

            ImGui::PushItemWidth(150);
            ImGui::SliderInt("Target Level", &m_privilegeTargetLevel, 1, 10);
            ImGui::PopItemWidth();

            if (ImGui::Button("Elevate Now", ImVec2(140, 25))) {
                try {
                    int result = Privilege::Elevate(m_privilegeTargetLevel, false);
                    if (result >= 7) {
                        LOG_INFO("[GUI] Elevation successful — level %d confirmed", result);
                    } else if (result > 0) {
                        LOG_WARN("[GUI] Elevation partial — level %d (target was %d)", result, m_privilegeTargetLevel);
                    } else {
                        LOG_ERROR("[GUI] Elevation failed — level unchanged");
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("[GUI] Elevation exception: %s", e.what());
                } catch (...) {
                    LOG_ERROR("[GUI] Elevation unknown exception");
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Install Detour", ImVec2(140, 25))) {
                try {
                    Privilege::InstallIdentityCheckDetour(m_privilegeTargetLevel);
                } catch (...) {
                    LOG_ERROR("[GUI] Detour installation failed");
                }
            }

            ImGui::SameLine();
            if (ImGui::Button("Remove Detour", ImVec2(140, 25))) {
                try {
                    Privilege::RemoveIdentityCheckDetour();
                } catch (...) {
                    LOG_ERROR("[GUI] Detour removal failed");
                }
            }

            ImGui::Spacing();
            ImGui::Checkbox("Auto-elevate on attach", &m_privilegeAutoElevate);
            ImGui::Checkbox("Bypass security checks", &m_privilegeBypassChecks);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                               "ScriptContext not resolved: %s", ctx.lastError.c_str());
            ImGui::TextDisabled("The target may not be initialized or the offset chain");
            ImGui::TextDisabled("may not match the current target version.");
        }
    } else {
        ImGui::TextDisabled("Attach to a process to configure privilege level.");
    }

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
//  Executer Tab (Lua IDE)
// ============================================================

void GUI::RenderExecuterTab() {
    ImGui::Begin("Executer");

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
    if (ImGui::Button("Run level_check", ImVec2(140, 25))) {
        LuaBridge::GetInstance().ExecuteFile("scripts/level_check.lua");
    }

    ImGui::SameLine();
    if (ImGui::Button("Load Script...", ImVec2(120, 25))) {
        if (!m_scriptPath.empty()) {
            LuaBridge::GetInstance().ExecuteFile(m_scriptPath);
        }
    }

    ImGui::SameLine();
    ImGui::PushItemWidth(200);
    ImGui::InputText("##scriptPath", &m_scriptPath[0], m_scriptPath.capacity(),
                     ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();

    ImGui::Separator();

    // ---- Console log output (reads from Logger ring buffer) ----
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Output");

    ImGui::BeginChild("ConsoleOutput", ImVec2(0, 0), true);

    static bool s_consoleAutoScroll = true;
    auto entries = Logging::Logger::GetInstance().GetEntries(200);
    for (const auto& entry : entries) {
        // Format: [HH:MM:SS.mmm] [LEVEL] message
        auto tt = std::chrono::system_clock::to_time_t(entry.timestamp);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.timestamp.time_since_epoch()) % 1000;
        char timeBuf[16];
        std::tm tm{};
        localtime_s(&tm, &tt);
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        switch (entry.level) {
            case Logging::LogLevel::Trace: color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); break;
            case Logging::LogLevel::Debug: color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); break;
            case Logging::LogLevel::Info:  color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break;
            case Logging::LogLevel::Warn:  color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); break;
            case Logging::LogLevel::Error: color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); break;
            case Logging::LogLevel::Fatal: color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); break;
        }

        ImGui::TextColored(color, "[%s.%03lld] [%s] %s",
                           timeBuf, ms.count(),
                           Logging::LogLevelToString(entry.level),
                           entry.message.c_str());
    }

    if (s_consoleAutoScroll) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();

    ImGui::End();
}

void GUI::ExecuteLuaInput() {
    std::string code(m_luaInputBuffer);
    if (code.empty()) return;

    // Echo to console via Logger
    LOG_INFO("> %s", code.c_str());
    m_consoleHistory.push_back(code);
    m_historyPos = -1;

    // Execute
    auto& bridge = LuaBridge::GetInstance();
    if (bridge.ExecuteString(code)) {
        LOG_INFO("[LuaConsole] Executed successfully");
    } else {
        LOG_ERROR("[LuaConsole] Execution failed");
    }

    // Cleanup
    std::memset(m_luaInputBuffer, 0, sizeof(m_luaInputBuffer));
}

// ============================================================
//  Log Panel
// ============================================================

void GUI::RenderErrorLoggerTab() {
    ImGui::Begin("Error Logger");

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Event Log");

    ImGui::Separator();

    ImGui::BeginChild("LogScroll", ImVec2(0, 0), true);

    // Filter checkboxes by log level
    static bool showTrace = false;
    static bool showDebug = true;
    static bool showInfo = true;
    static bool showWarn = true;
    static bool showError = true;
    static bool showFatal = true;

    ImGui::Checkbox("Trace", &showTrace);
    ImGui::SameLine();
    ImGui::Checkbox("Debug", &showDebug);
    ImGui::SameLine();
    ImGui::Checkbox("Info", &showInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &showWarn);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &showError);
    ImGui::SameLine();
    ImGui::Checkbox("Fatal", &showFatal);

    ImGui::Separator();

    auto entries = Logging::Logger::GetInstance().GetEntries(200);
    for (const auto& entry : entries) {
        // Filter by level
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

        ImVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
        switch (entry.level) {
            case Logging::LogLevel::Trace: color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); break;
            case Logging::LogLevel::Debug: color = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); break;
            case Logging::LogLevel::Info:  color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); break;
            case Logging::LogLevel::Warn:  color = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); break;
            case Logging::LogLevel::Error: color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); break;
            case Logging::LogLevel::Fatal: color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); break;
        }

        ImGui::TextColored(color, "[%s] %s",
                           Logging::LogLevelToString(entry.level),
                           entry.message.c_str());
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
                LOG_INFO("%s", w.text.c_str());

                break;
            }

            case GuiWidget::Type::Text: {
                // Text widgets are rendered inline in their tab
                // For now, add to log
                LOG_DEBUG("[TEXT] %s: %s", w.tabName.c_str(), w.text.c_str());

                break;
            }

            case GuiWidget::Type::Slider: {
                // Slider widget — value is handled by the Lua callback system
                LOG_DEBUG("[Widget] Slider '%s/%s' = %.3f",
                          w.tabName.c_str(), w.label.c_str(), w.values[0]);
                break;
            }

            case GuiWidget::Type::Checkbox: {
                // Checkbox widget — state is handled by the Lua callback system
                LOG_DEBUG("[Widget] Checkbox '%s/%s' = %s",
                          w.tabName.c_str(), w.label.c_str(),
                          w.values[0] > 0.5f ? "on" : "off");
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

        // Persist privilege elevation preferences
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

        // Restore privilege elevation preferences
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
