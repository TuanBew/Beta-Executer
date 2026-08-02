/**
 * Universal Hub — Main Entry Point
 *
 * An educational process interaction and scripting framework.
 * Attaches to a target process to demonstrate memory I/O, LuaU scripting,
 * and modular GUI-based automation control.
 *
 * Default mode launches the Dear ImGui control panel. Pass --console
 * to use the legacy text-based REPL instead.
 *
 * Initialization order:
 *   1. Logger        — structured logging to file, GUI ring buffer, debug output
 *   2. CrashHandler  — SEH/C++/signal handlers for crash diagnostics
 *   3. LuaBridge     — LuaU VM with sandbox + bridge functions
 *   4. Engine        — optional auto-attach via --attach flag (triggers Privilege::AutoElevateOnAttach)
 *   5. GUI           — GLFW window + ImGui docking (default) or console REPL (--console)
 *
 * Shutdown order (reverse):
 *   LuaBridge → Engine → Privilege → CrashHandler → Logger
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — intended for use with test.exe
 * in a controlled offline environment.
 */

#include <string>
#include <cstdlib>
#include <iostream>

#include "Logging/Logger.h"
#include "Logging/CrashHandler.h"
#include "Core/Engine.h"
#include "Core/Memory.h"
#include "Core/Bootstrap.h"
#include "Core/PrivilegeElevation.h"
#include "Lua/LuaBridge.h"
#include "GUI/GUI.h"
#include "Protocol/PipeServer.h"
#include "Injector/CapcomDriver.h"
#include "Injector/KernelExec.h"

static std::string g_injectMode;   // "legacy" (default) or "manual"

static void PrintUsage() {
    printf("\n");
    printf("  Universal Hub — Automation Framework\n");
    printf("  ======================================\n\n");
    printf("  Usage:\n");
    printf("    UniversalHub.exe                     Launch GUI control panel (default)\n");
    printf("    UniversalHub.exe --console           Launch text-based REPL\n");
    printf("    UniversalHub.exe --attach <PID|name> Auto-attach on startup\n");
    printf("    UniversalHub.exe --console --attach <PID|name> --run <script.lua>\n");
    printf("                                          Headless: attach, run script, exit (no REPL/GUI)\n\n");
    printf("  Console Commands:\n");
    printf("    attach <process>   Attach to a process by name or PID\n");
    printf("    detach             Detach from current process\n");
    printf("    read <addr> <type> Read memory (type: i32, f32, u8, str)\n");
    printf("    write <addr> <val> Write memory (same types as read)\n");
    printf("    module <name>      Get base address of a module\n");
    printf("    execute <path>     Run a Lua script file\n");
    printf("    bootstrap <dll>    Load DLL into target (educational demo)\n");
    printf("    help               Show this help\n");
    printf("    exit               Quit\n\n");
}

// ---- Legacy Console REPL ----
// Extracted from the original main() body for the --console fallback path.

static void RunConsoleRepl() {
    PrintUsage();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        size_t firstSpace = line.find(' ');
        std::string cmd = line.substr(0, firstSpace);
        std::string args = (firstSpace != std::string::npos)
            ? line.substr(firstSpace + 1) : "";

        if (cmd == "exit" || cmd == "quit") {
            break;
        }
        else if (cmd == "help") {
            PrintUsage();
        }
        else if (cmd == "attach") {
            if (args.empty()) {
                LOG_WARN("Usage: attach <process_name or PID>");
                continue;
            }
            try {
                DWORD pid = std::stoul(args);
                Engine::GetInstance().AttachToProcess(pid);
            } catch (...) {
                Engine::GetInstance().AttachToProcess(args);
            }
        }
        else if (cmd == "detach") {
            Engine::GetInstance().Detach();
        }
        else if (cmd == "read") {
            size_t sp2 = args.find(' ');
            if (sp2 == std::string::npos) {
                LOG_WARN("Usage: read <hex_address> <type>");
                continue;
            }
            try {
                uintptr_t addr = std::stoull(args.substr(0, sp2), nullptr, 16);
                std::string type = args.substr(sp2 + 1);

                if (type == "i32" || type == "int32") {
                    int32_t v = Memory::Read<int32_t>(addr);
                    LOG_INFO("Read [0x%llX] int32 = %d", addr, v);
                }
                else if (type == "f32" || type == "float") {
                    float v = Memory::Read<float>(addr);
                    LOG_INFO("Read [0x%llX] float = %.3f", addr, v);
                }
                else if (type == "u8" || type == "uint8") {
                    int v = static_cast<int>(Memory::Read<uint8_t>(addr));
                    LOG_INFO("Read [0x%llX] uint8 = %d", addr, v);
                }
                else if (type == "str" || type == "string") {
                    std::string s = Memory::ReadString(addr);
                    LOG_INFO("Read [0x%llX] string = \"%s\"", addr, s.c_str());
                }
                else {
                    LOG_WARN("Unknown type: %s (use: i32, f32, u8, str)", type.c_str());
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Read failed: %s", e.what());
            }
        }
        else if (cmd == "write") {
            size_t sp2 = args.find(' ');
            if (sp2 == std::string::npos) {
                LOG_WARN("Usage: write <hex_address> <value> [type=f32]");
                continue;
            }
            size_t sp3 = args.find(' ', sp2 + 1);
            std::string addrStr = args.substr(0, sp2);
            std::string valStr = args.substr(sp2 + 1, sp3 - sp2 - 1);
            std::string type = (sp3 != std::string::npos) ? args.substr(sp3 + 1) : "f32";

            try {
                uintptr_t addr = std::stoull(addrStr, nullptr, 16);

                if (type == "i32") {
                    Memory::Write<int32_t>(addr, std::stoi(valStr));
                } else if (type == "f32" || type == "float") {
                    Memory::Write<float>(addr, std::stof(valStr));
                } else if (type == "u8") {
                    Memory::Write<uint8_t>(addr, static_cast<uint8_t>(std::stoi(valStr)));
                } else if (type == "i64") {
                    Memory::Write<int64_t>(addr, std::stoll(valStr));
                }

                LOG_INFO("Wrote %s (%s) to 0x%llX", valStr.c_str(), type.c_str(), addr);
            } catch (const std::exception& e) {
                LOG_ERROR("Write failed: %s", e.what());
            }
        }
        else if (cmd == "module") {
            if (args.empty()) {
                LOG_WARN("Usage: module <name> (e.g., test.exe)");
                continue;
            }
            uintptr_t base = Memory::GetModuleBaseAddress(args);
            if (base) {
                LOG_INFO("%s base: 0x%llX", args.c_str(), base);
            } else {
                LOG_INFO("Module '%s' not found", args.c_str());
            }
        }
        else if (cmd == "execute") {
            if (args.empty()) {
                LOG_WARN("Usage: execute <script_path>");
                continue;
            }
            if (LuaBridge::GetInstance().ExecuteFile(args)) {
                LOG_INFO("Script executed: %s", args.c_str());
            } else {
                LOG_ERROR("Script execution failed: %s", args.c_str());
            }
        }
        else if (cmd == "bootstrap") {
            if (args.empty()) {
                LOG_WARN("Usage: bootstrap <dll_path>");
                continue;
            }
            LOG_INFO("FOR EDUCATIONAL DEMONSTRATION ONLY");
            if (g_injectMode == "manual") {
                Bootstrap::ManualMapIntoProcess(Engine::GetInstance().GetPid(), args);
            } else {
                Bootstrap::LoadIntoProcess(args);
            }
        }
        else if (cmd == "crash") {
            LOG_INFO("Triggering test crash in 1 second...");
            Logging::CrashHandler::GetInstance().TriggerCrash("Test crash from console");
        }
        else {
            LOG_WARN("Unknown command: %s (type 'help')", cmd.c_str());
        }
    }
}

// ---- Main ----

int main(int argc, char* argv[]) {
    // ---- 1. Logger ----
    Logging::Logger::GetInstance().Initialize();

    // ---- 2. CrashHandler ----
    Logging::CrashHandler::GetInstance().Install();

    LOG_INFO("Universal Hub v1.0.0 — Automation Framework");
    LOG_INFO("FOR EDUCATIONAL DEMONSTRATION ONLY");

    // ---- 3. LuaBridge (LuaU VM + sandbox + bridges) ----
    if (!LuaBridge::GetInstance().Initialize()) {
        LOG_WARN("[Main] LuaBridge failed to initialize — scripting will be unavailable");
    }

    // ---- Load Capcom driver (Phase 2 kernel R/W) ----
    if (!CapcomDriver::GetInstance().LoadDriver()) {
        LOG_WARN("[Main] Capcom driver not loaded — falling back to user-mode RPM/WPM");
    } else {
        LOG_INFO("[Main] Capcom.sys loaded — kernel R/W available");
        KernelExec::GetInstance().Initialize();
    }

    // ---- 4. Auto-attach via --attach flag (accepts PID or process name) ----
    // Note: Engine::AttachToProcess now calls Privilege::AutoElevateOnAttach() internally.
    bool attached = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--attach" && i + 1 < argc) {
            std::string target = argv[++i];
            bool ok = false;
            try {
                DWORD pid = std::stoul(target);
                ok = Engine::GetInstance().AttachToProcess(pid);
            } catch (...) {
                ok = Engine::GetInstance().AttachToProcess(target);
            }
            if (ok) {
                LOG_INFO("Successfully attached to '%s'", target.c_str());
                attached = true;
                // Start pipe server for payload communication
                PipeServer::GetInstance().Initialize();
            } else {
                LOG_ERROR("Failed to attach to '%s'. Try running as Administrator.", target.c_str());
            }
        }
    }

    // ---- Determine mode: GUI (default), console (--console), or headless one-shot (--run / --run-pipe) ----
    bool useConsole = false;
    std::string runScript;
    std::string runPipeScript;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--console") {
            useConsole = true;
        } else if (a == "--run" && i + 1 < argc) {
            runScript = argv[++i];
        } else if (a == "--run-pipe" && i + 1 < argc) {
            runPipeScript = argv[++i];
        } else if (a == "--inject" && i + 1 < argc) {
            g_injectMode = argv[++i];  // "manual" or "legacy"
        }
    }

    if (!runScript.empty()) {
        // ---- Headless one-shot: attach (already done above), run script, exit ----
        // No REPL, no GUI — designed for scripted/automated CLI invocation.
        if (!attached) {
            LOG_WARN("[Main] --run given without a successful --attach — script will run unattached");
        }
        LOG_INFO("[Main] Headless run: executing '%s'", runScript.c_str());
        if (LuaBridge::GetInstance().ExecuteFile(runScript)) {
            LOG_INFO("[Main] Headless run complete: %s", runScript.c_str());
        } else {
            LOG_ERROR("[Main] Headless run failed: %s", runScript.c_str());
        }
    } else if (!runPipeScript.empty()) {
        // ---- Headless pipe one-shot: run script via pipe to payload DLL ----
        if (!attached || !PipeServer::GetInstance().IsConnected()) {
            LOG_ERROR("[Main] --run-pipe requires --attach with pipe-connected target");
        } else {
            LOG_INFO("[Main] Pipe run: executing '%s'", runPipeScript.c_str());
            std::string errorMsg;
            if (PipeServer::GetInstance().ExecuteScript(runPipeScript, errorMsg)) {
                LOG_INFO("[Main] Pipe run complete: %s", runPipeScript.c_str());
            } else {
                LOG_ERROR("[Main] Pipe run failed: %s", errorMsg.c_str());
            }
        }
    } else if (useConsole) {
        // ---- Legacy Console REPL ----
        RunConsoleRepl();
    } else {
        // ---- GUI Mode (default) ----
        if (GUI::GetInstance().Initialize("Universal Hub", 1280, 720)) {
            LOG_INFO("[Main] Entering GUI mode — press INSERT to toggle visibility");
            GUI::GetInstance().Run();
        } else {
            LOG_WARN("[Main] GUI initialization failed (headless or no GLFW?) — falling back to console REPL");
            RunConsoleRepl();
        }
    }

    // ---- Clean shutdown (reverse init order) ----
    LOG_INFO("[Main] Shutting down...");

    LuaBridge::GetInstance().Shutdown();
    PipeServer::GetInstance().Shutdown();
    Engine::GetInstance().Detach();
    KernelExec::GetInstance().Shutdown();   // restore HalDispatchTable before driver unload
    CapcomDriver::GetInstance().UnloadDriver();
    Privilege::Cleanup();
    Logging::CrashHandler::GetInstance().Uninstall();
    Logging::Logger::GetInstance().Shutdown();

    return 0;
}
