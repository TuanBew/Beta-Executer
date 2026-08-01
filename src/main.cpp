/**
 * Universal Hub — Main Entry Point
 *
 * An educational process interaction and scripting framework.
 * Attaches to a target process to demonstrate memory I/O, LuaU scripting,
 * and modular GUI-based automation control.
 *
 * Initialization order:
 *   1. Logger     — structured logging to file, GUI ring buffer, debug output
 *   2. CrashHandler — SEH/C++/signal handlers for crash diagnostics
 *   3. Engine     — optional auto-attach via --attach flag
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — intended for use with test.exe
 * in a controlled offline environment.
 */

#include <string>
#include "Logging/Logger.h"
#include "Logging/CrashHandler.h"
#include "Core/Engine.h"
#include "Core/Memory.h"
#include "Core/Bootstrap.h"

void PrintUsage() {
    // PrintUsage still uses stdout directly since it's the interactive help text
    // and not a log event per se. The banner and CLI output go through Logger.
    printf("\n");
    printf("  Universal Hub — Automation Dashboard\n");
    printf("  ======================================\n\n");
    printf("  Commands:\n");
    printf("    attach <process>   Attach to a process by name or PID\n");
    printf("    detach             Detach from current process\n");
    printf("    read <addr> <type> Read memory (type: i32, f32, u8, str)\n");
    printf("    write <addr> <val> Write memory (same types as read)\n");
    printf("    module <name>      Get base address of a module\n");
    printf("    bootstrap <dll>    Load DLL into target (educational demo)\n");
    printf("    gui                Launch the GUI control panel\n");
    printf("    help               Show this help\n");
    printf("    exit               Quit\n\n");
}

int main(int argc, char* argv[]) {
    // ---- Phase 7: Initialize logging & crash handling first ----
    Logging::Logger::GetInstance().Initialize();
    Logging::CrashHandler::GetInstance().Install();

    LOG_INFO("Universal Hub v1.0.0 — Automation Framework");
    LOG_INFO("FOR EDUCATIONAL DEMONSTRATION ONLY");

    // If command-line PID provided, attach immediately
    if (argc >= 3 && std::string(argv[1]) == "--attach") {
        DWORD pid = std::stoul(argv[2]);
        if (!Engine::GetInstance().AttachToProcess(pid)) {
            LOG_ERROR("Failed to attach to PID %lu. Try running as Administrator.", pid);
            Logging::CrashHandler::GetInstance().Uninstall();
            Logging::Logger::GetInstance().Shutdown();
            return 1;
        }
        LOG_INFO("Successfully attached to PID %lu", pid);
    }

    PrintUsage();

    // Interactive console loop (Phase 1 testing)
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // Parse: command arg1 [arg2]
        size_t firstSpace = line.find(' ');
        std::string cmd = line.substr(0, firstSpace);
        std::string args = (firstSpace != std::string::npos)
            ? line.substr(firstSpace + 1) : "";

        if (cmd == "exit" || cmd == "quit") {
            Engine::GetInstance().Detach();
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
            // Try numeric PID first, then process name
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
            // Parse: read <address> <type>
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
            // Parse: write <address> <value> [type]
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
        else if (cmd == "bootstrap") {
            if (args.empty()) {
                LOG_WARN("Usage: bootstrap <dll_path>");
                continue;
            }
            LOG_INFO("FOR EDUCATIONAL DEMONSTRATION ONLY");
            Bootstrap::LoadIntoProcess(args);
        }
        else if (cmd == "gui") {
            LOG_INFO("GUI mode will be available in Phase 3");
        }
        else if (cmd == "crash") {
            // Hidden command: test crash handler
            LOG_INFO("Triggering test crash in 1 second...");
            Logging::CrashHandler::GetInstance().TriggerCrash("Test crash from console");
        }
        else {
            LOG_WARN("Unknown command: %s (type 'help')", cmd.c_str());
        }
    }

    // ---- Clean shutdown ----
    Logging::CrashHandler::GetInstance().Uninstall();
    Logging::Logger::GetInstance().Shutdown();
    return 0;
}
