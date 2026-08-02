// src/Injector/ManualMapInjector.h
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <map>

/**
 * Parsed representation of a PE import entry.
 */
struct ImportEntry {
    std::string dllName;
    std::string funcName;
    uint16_t    hint = 0;
    bool        byOrdinal = false;
    uintptr_t   resolvedAddr = 0; // resolved at injection time
};

/**
 * ManualMapInjector — full PE manual mapper operating through kernel R/W.
 *
 * Replaces CreateRemoteThread+LoadLibraryA injection with a manual map that:
 *   1. Reads DLL file from disk
 *   2. Parses PE headers, section table, import table, relocation table
 *   3. Allocates memory in target via KernelExec → ZwAllocateVirtualMemory
 *   4. Writes PE headers, sections via Capcom writes
 *   5. Applies relocation fixups (delta = actual - preferred base)
 *   6. Resolves imports by walking target's PEB/Ldr module list via Capcom reads
 *   7. Executes entry point via APC on a target thread
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — manual mapping bypasses module
 * visibility lists; use only in controlled offline environments.
 */
class ManualMapInjector {
public:
    static ManualMapInjector& GetInstance();

    /**
     * Full manual map pipeline.
     * @param pid       Target process ID.
     * @param dllPath   Path to the DLL file on disk.
     * @return true if injection succeeded and entry point executed.
     */
    bool Inject(DWORD pid, const std::string& dllPath);

    /**
     * @return The base address the DLL was mapped at in the target (valid after Inject).
     */
    uintptr_t GetMappedBase() const { return m_mappedBase; }

private:
    ManualMapInjector() = default;
    ~ManualMapInjector() = default;
    ManualMapInjector(const ManualMapInjector&) = delete;
    ManualMapInjector& operator=(const ManualMapInjector&) = delete;

    // ---- PE Parsing ----
    bool ParsePE(const std::vector<uint8_t>& dllBytes);

    // ---- Memory Allocation ----
    bool AllocateTargetMemory(DWORD pid, uintptr_t preferredBase);
    bool CopyHeadersAndSections(DWORD pid);
    bool SetSectionProtection(DWORD pid);

    // ---- Import Resolution ----
    bool ResolveImports(DWORD pid);
    uintptr_t FindModuleInTarget(DWORD pid, const std::string& moduleName);
    uintptr_t FindExportInTarget(DWORD pid, uintptr_t moduleBase, const std::string& funcName);
    uintptr_t ResolveForwardedExport(DWORD pid, const std::string& forwarder);

    // ---- Relocations ----
    bool ApplyRelocations(DWORD pid);

    // ---- Entry Point Execution ----
    bool ExecuteEntryPoint(DWORD pid);
    bool QueueUserApcToTarget(DWORD pid, uintptr_t apcRoutine, uintptr_t argument);

    // ---- Parsed PE Data ----
    std::vector<uint8_t> m_rawDll;

    IMAGE_DOS_HEADER           m_dosHeader{};
    IMAGE_NT_HEADERS64         m_ntHeaders{};
    std::vector<IMAGE_SECTION_HEADER> m_sections;

    uintptr_t m_preferredBase    = 0;
    uintptr_t m_mappedBase       = 0;
    size_t    m_imageSize        = 0;
    uintptr_t m_entryPointRva    = 0;

    // Parsed imports: dllName → [ImportEntry]
    std::map<std::string, std::vector<ImportEntry>> m_imports;
};
