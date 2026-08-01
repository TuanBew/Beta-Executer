/**
 * Universal Hub — Main Entry Point
 *
 * An educational process interaction and scripting framework.
 * Attaches to a target process to demonstrate memory I/O, LuaU scripting,
 * and modular GUI-based automation control.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — intended for use with test.exe
 * in a controlled offline environment.
 */

#include <iostream>
#include <string>
#include "Core/Engine.h"
#include "Core/Memory.h"
#include "Core/Bootstrap.h"

void PrintUsage() {
    std::cout << "\n";
    std::cout << "  Universal Hub — Automation Dashboard\n";
    std::cout << "  ======================================\n\n";
    std::cout << "  Commands:\n";
    std::cout << "    attach <process>   Attach to a process by name or PID\n";
    std::cout << "    detach             Detach from current process\n";
    std::cout << "    read <addr> <type> Read memory (type: i32, f32, u8, str)\n";
    std::cout << "    write <addr> <val> Write memory (same types as read)\n";
    std::cout << "    module <name>      Get base address of a module\n";
    std::cout << "    bootstrap <dll>    Load DLL into target (educational demo)\n";
    std::cout << "    gui                Launch the GUI control panel\n";
    std::cout << "    help               Show this help\n";
    std::cout << "    exit               Quit\n\n";
}

/**
 * Phase 1 demonstration: attach to a target process and perform basic
 * memory read/write operations from the console.
 *
 * In Phase 3+, this will be replaced by the Dear ImGui GUI loop.
 */
int main(int argc, char* argv[]) {
    std::cout << "Universal Hub v1.0.0 — Automation Framework\n";
    std::cout << "FOR EDUCATIONAL DEMONSTRATION ONLY\n\n";

    // If command-line PID provided, attach immediately
    if (argc >= 3 && std::string(argv[1]) == "--attach") {
        DWORD pid = std::stoul(argv[2]);
        if (!Engine::GetInstance().AttachToProcess(pid)) {
            std::cerr << "Failed to attach. Try running as Administrator." << std::endl;
            return 1;
        }
        std::cout << "Successfully attached to PID " << pid << std::endl;
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
                std::cerr << "Usage: attach <process_name or PID>" << std::endl;
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
                std::cerr << "Usage: read <hex_address> <type>" << std::endl;
                continue;
            }
            try {
                uintptr_t addr = std::stoull(args.substr(0, sp2), nullptr, 16);
                std::string type = args.substr(sp2 + 1);

                if (type == "i32" || type == "int32") {
                    int32_t v = Memory::Read<int32_t>(addr);
                    std::cout << "  [0x" << std::hex << addr << std::dec
                              << "] int32 = " << v << std::endl;
                }
                else if (type == "f32" || type == "float") {
                    float v = Memory::Read<float>(addr);
                    std::cout << "  [0x" << std::hex << addr << std::dec
                              << "] float = " << v << std::endl;
                }
                else if (type == "u8" || type == "uint8") {
                    int v = static_cast<int>(Memory::Read<uint8_t>(addr));
                    std::cout << "  [0x" << std::hex << addr << std::dec
                              << "] uint8 = " << v << std::endl;
                }
                else if (type == "str" || type == "string") {
                    std::string s = Memory::ReadString(addr);
                    std::cout << "  [0x" << std::hex << addr << std::dec
                              << "] string = \"" << s << "\"" << std::endl;
                }
                else {
                    std::cerr << "Unknown type: " << type
                              << " (use: i32, f32, u8, str)" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
        else if (cmd == "write") {
            // Parse: write <address> <value> [type]
            size_t sp2 = args.find(' ');
            if (sp2 == std::string::npos) {
                std::cerr << "Usage: write <hex_address> <value> [type=f32]" << std::endl;
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

                std::cout << "  Wrote " << valStr << " (" << type
                          << ") to 0x" << std::hex << addr << std::dec << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
            }
        }
        else if (cmd == "module") {
            if (args.empty()) {
                std::cerr << "Usage: module <name> (e.g., test.exe)" << std::endl;
                continue;
            }
            uintptr_t base = Memory::GetModuleBaseAddress(args);
            if (base) {
                std::cout << "  " << args << " base: 0x"
                          << std::hex << base << std::dec << std::endl;
            } else {
                std::cout << "  Module '" << args << "' not found" << std::endl;
            }
        }
        else if (cmd == "bootstrap") {
            if (args.empty()) {
                std::cerr << "Usage: bootstrap <dll_path>" << std::endl;
                continue;
            }
            std::cout << "  FOR EDUCATIONAL DEMONSTRATION ONLY" << std::endl;
            Bootstrap::LoadIntoProcess(args);
        }
        else if (cmd == "gui") {
            std::cout << "  GUI mode will be available in Phase 3" << std::endl;
        }
        else {
            std::cerr << "Unknown command: " << cmd << " (type 'help')" << std::endl;
        }
    }

    return 0;
}
