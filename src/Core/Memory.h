#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <windows.h>
#include "Engine.h"

/**
 * Memory I/O module — provides templated read/write access to the target
 * process's virtual memory space using the Win32 ReadProcessMemory and
 * WriteProcessMemory APIs.
 *
 * All operations go through the Engine singleton's process handle.
 * Addresses are absolute virtual addresses in the target process.
 */

namespace Memory {

    // ---- Type codes for Lua bridge (used in Phase 2+) ----

    enum class TypeCode : int {
        Int32 = 0,
        Float = 1,
        Double = 2,
        Int64 = 3,
        Uint8 = 4,
        Bool = 5,
        String = 6,
        Int16 = 7,
        Uint32 = 8
    };

    // ---- Templated Read ----

    /**
     * Read a value of type T from the target process at the given virtual address.
     * Blocks until the read completes or fails.
     *
     * @tparam T     The type to read (must be trivially copyable).
     * @param address Absolute virtual address in the target process.
     * @return The value read.
     * @throws std::runtime_error if not attached or read fails.
     */
    template<typename T>
    T Read(uintptr_t address) {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            throw std::runtime_error("Memory::Read: Not attached to a process");
        }

        T value{};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(engine.GetProcessHandle(),
                               reinterpret_cast<LPCVOID>(address),
                               &value, sizeof(T), &bytesRead) ||
            bytesRead != sizeof(T)) {
            throw std::runtime_error("Memory::Read: ReadProcessMemory failed at 0x" +
                                     std::to_string(address));
        }
        return value;
    }

    // ---- Templated Write ----

    /**
     * Write a value of type T to the target process at the given virtual address.
     * Blocks until the write completes or fails.
     *
     * @tparam T     The type to write (must be trivially copyable).
     * @param address Absolute virtual address in the target process.
     * @param value  The value to write.
     * @throws std::runtime_error if not attached or write fails.
     */
    template<typename T>
    void Write(uintptr_t address, T value) {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            throw std::runtime_error("Memory::Write: Not attached to a process");
        }

        SIZE_T bytesWritten = 0;
        if (!WriteProcessMemory(engine.GetProcessHandle(),
                                reinterpret_cast<LPVOID>(address),
                                &value, sizeof(T), &bytesWritten) ||
            bytesWritten != sizeof(T)) {
            throw std::runtime_error("Memory::Write: WriteProcessMemory failed at 0x" +
                                     std::to_string(address));
        }
    }

    // ---- String Read ----

    /**
     * Read a null-terminated UTF-8 string from the target process.
     *
     * @param address Starting address of the string in the target.
     * @param maxLen  Maximum number of bytes to read (safety limit).
     * @return The string content (without null terminator).
     */
    inline std::string ReadString(uintptr_t address, size_t maxLen = 256) {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            throw std::runtime_error("Memory::ReadString: Not attached to a process");
        }

        std::vector<char> buffer(maxLen + 1, 0);
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(engine.GetProcessHandle(),
                               reinterpret_cast<LPCVOID>(address),
                               buffer.data(), maxLen, &bytesRead)) {
            throw std::runtime_error("Memory::ReadString: ReadProcessMemory failed at 0x" +
                                     std::to_string(address));
        }

        // Find null terminator
        size_t len = 0;
        for (size_t i = 0; i < bytesRead; ++i) {
            if (buffer[i] == '\0') {
                len = i;
                break;
            }
        }
        if (len == 0 && bytesRead > 0 && buffer[0] != '\0') {
            len = bytesRead; // No null found, use all bytes
        }

        return std::string(buffer.data(), len);
    }

    // ---- Bulk Read ----

    inline std::vector<uint8_t> ReadBytes(uintptr_t address, size_t count) {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            throw std::runtime_error("Memory::ReadBytes: Not attached to a process");
        }

        std::vector<uint8_t> buffer(count, 0);
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(engine.GetProcessHandle(),
                               reinterpret_cast<LPCVOID>(address),
                               buffer.data(), count, &bytesRead)) {
            throw std::runtime_error("Memory::ReadBytes: ReadProcessMemory failed at 0x" +
                                     std::to_string(address));
        }
        buffer.resize(bytesRead);
        return buffer;
    }

    inline std::string HexDump(uintptr_t address, size_t size) {
        std::string result;
        result.reserve(size * 5);

        try {
            auto bytes = ReadBytes(address, size);
            char line[128];
            for (size_t off = 0; off < bytes.size(); off += 16) {
                size_t lineLen = (bytes.size() - off < 16) ? bytes.size() - off : 16;
                int pos = sprintf(line, "%016llX  ", (unsigned long long)(address + off));

                for (size_t i = 0; i < 16; ++i) {
                    if (i == 8) line[pos++] = ' ';
                    if (i < lineLen)
                        pos += sprintf(line + pos, "%02X ", bytes[off + i]);
                    else
                        pos += sprintf(line + pos, "   ");
                }

                line[pos++] = ' ';
                for (size_t i = 0; i < lineLen; ++i) {
                    uint8_t c = bytes[off + i];
                    line[pos++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                }
                line[pos++] = '\n';
                line[pos] = '\0';
                result += line;
            }
        } catch (const std::exception& e) {
            result = "HexDump failed at 0x" + std::to_string(address) + ": " + e.what() + "\n";
        }
        return result;
    }

    /**
     * Get the base address of a loaded module in the target process.
     * Walks the module list of the target to find the named module.
     *
     * @param moduleName  The module filename (e.g., "test.exe" or "kernel32.dll").
     * @return The base address, or 0 if not found.
     */
    inline uintptr_t GetModuleBaseAddress(const std::string& moduleName) {
        auto& engine = Engine::GetInstance();
        if (!engine.IsAttached()) {
            return 0;
        }

        HANDLE hSnapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, engine.GetPid());
        if (hSnapshot == INVALID_HANDLE_VALUE) {
            return 0;
        }

        uintptr_t base = 0;
        MODULEENTRY32 me32{};
        me32.dwSize = sizeof(MODULEENTRY32);

        if (Module32First(hSnapshot, &me32)) {
            do {
                if (_stricmp(me32.szModule, moduleName.c_str()) == 0) {
                    base = reinterpret_cast<uintptr_t>(me32.modBaseAddr);
                    break;
                }
            } while (Module32Next(hSnapshot, &me32));
        }

        CloseHandle(hSnapshot);
        return base;
    }

} // namespace Memory
