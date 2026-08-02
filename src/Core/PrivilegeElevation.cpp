/**
 * PrivilegeElevation.cpp — Script Identity Elevation Implementation (v2)
 *
 * v2 changes: multi-path DataModel resolution, pointer validation at every
 * step, ScriptContext children-walk fallback, configurable retry with
 * backoff, INFO-level diagnostic logging throughout.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — controlled offline environment.
 */

#include "Core/PrivilegeElevation.h"
#include "Core/Memory.h"
#include "Core/offsets.h"
#include "Logging/Logger.h"

#include <fstream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace Privilege {

// ---- Module-level state ----

static uintptr_t s_detourAddress = 0;
static uint8_t   s_originalBytes[6] = {};
static bool      s_detourActive = false;

// ---- Helpers ----

static bool IsValidPointer(uintptr_t ptr) {
    return ptr >= 0x10000 && ptr < 0x7FFFFFFFFFFF;
}

static nlohmann::json LoadConfigFile(const std::string& path) {
    try {
        std::ifstream f(path);
        if (!f.is_open()) return {};
        nlohmann::json j;
        f >> j;
        return j;
    } catch (...) {
        return {};
    }
}

static std::string ReadClassName(uintptr_t objectAddr) {
    try {
        uintptr_t classDesc = Memory::Read<uintptr_t>(objectAddr + offsets::ClassDescriptor);
        if (!IsValidPointer(classDesc)) return "";
        uintptr_t namePtr = Memory::Read<uintptr_t>(classDesc + offsets::ClassDescriptorToClassName);
        if (!IsValidPointer(namePtr)) return "";
        return Memory::ReadString(namePtr, 64);
    } catch (...) {
        return "";
    }
}

// SSO-aware MSVC x64 std::string reader.
// Layout: [union{char buf[16], char* ptr}][size_t len @ +0x10][size_t cap @ +0x18]
// If cap < 16 the string data lives inline in buf; otherwise behind the pointer.
static std::string ReadMSVCString(uintptr_t stringAddr) {
    try {
        size_t capacity = Memory::Read<size_t>(stringAddr + 0x18);
        size_t length   = Memory::Read<size_t>(stringAddr + 0x10);
        if (length == 0 || length > 4096) return "";

        if (capacity < 16) {
            size_t toRead = (length < 15) ? length : 15;
            auto bytes = Memory::ReadBytes(stringAddr, toRead + 1);
            if (bytes.empty()) return "";
            bytes.push_back(0);
            return std::string(reinterpret_cast<const char*>(bytes.data()), toRead);
        }

        uintptr_t ptr = Memory::Read<uintptr_t>(stringAddr);
        if (!IsValidPointer(ptr)) return "";
        return Memory::ReadString(ptr, (length < 256) ? length : 256);
    } catch (...) {
        return "";
    }
}

static void DumpHex(uintptr_t address, size_t numBytes, const char* label) {
    LOG_INFO("[Privilege] === HexDump: %s @ 0x%llX (%zu bytes) ===",
             label, (unsigned long long)address, numBytes);
    try {
        auto bytes = Memory::ReadBytes(address, numBytes);
        for (size_t off = 0; off < bytes.size(); off += 16) {
            char hex[64] = {};
            char ascii[20] = {};
            size_t lineLen = (bytes.size() - off < 16) ? bytes.size() - off : 16;
            for (size_t i = 0; i < lineLen; ++i) {
                sprintf(hex + i * 3, "%02X ", bytes[off + i]);
                ascii[i] = (bytes[off + i] >= 0x20 && bytes[off + i] < 0x7F)
                           ? (char)bytes[off + i] : '.';
            }
            ascii[lineLen] = '\0';
            LOG_INFO("[Privilege]   +0x%03X: %-48s  %s", (unsigned)off, hex, ascii);
        }
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   HexDump failed: %s", e.what());
    }
}

// ============================================================
//  Multi-Path DataModel Resolution
// ============================================================

static uintptr_t ResolveDataModelPathA(uintptr_t moduleBase) {
    LOG_INFO("[Privilege] Path A: FakeDataModel chain");

    uintptr_t fakeDMPtrAddr = moduleBase + offsets::FakeDataModelPointer;
    LOG_INFO("[Privilege]   step 1: reading [moduleBase+0x%llX] = [0x%llX]",
             (unsigned long long)offsets::FakeDataModelPointer,
             (unsigned long long)fakeDMPtrAddr);

    uintptr_t fakeDM = 0;
    try {
        fakeDM = Memory::Read<uintptr_t>(fakeDMPtrAddr);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   step 1 FAILED: ReadProcessMemory error: %s", e.what());
        return 0;
    }

    LOG_INFO("[Privilege]   step 1 result: FakeDataModel = 0x%llX", (unsigned long long)fakeDM);
    if (fakeDM == 0) {
        LOG_WARN("[Privilege]   step 1: FakeDataModel pointer is null");
        return 0;
    }
    if (!IsValidPointer(fakeDM)) {
        LOG_WARN("[Privilege]   step 1: FakeDataModel 0x%llX outside valid range — suspicious",
                 (unsigned long long)fakeDM);
        return 0;
    }

    LOG_INFO("[Privilege]   step 2: reading [FakeDataModel+0x%llX] = [0x%llX]",
             (unsigned long long)offsets::FakeDataModelToDataModel,
             (unsigned long long)(fakeDM + offsets::FakeDataModelToDataModel));

    uintptr_t dm = 0;
    try {
        dm = Memory::Read<uintptr_t>(fakeDM + offsets::FakeDataModelToDataModel);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   step 2 FAILED: ReadProcessMemory error: %s", e.what());
        return 0;
    }

    LOG_INFO("[Privilege]   step 2 result: DataModel = 0x%llX", (unsigned long long)dm);
    if (dm == 0 || !IsValidPointer(dm)) {
        LOG_WARN("[Privilege]   step 2: DataModel 0x%llX invalid", (unsigned long long)dm);
        return 0;
    }

    // Validate: check that DataModel has children and looks like a DataModel
    try {
        uintptr_t children = Memory::Read<uintptr_t>(dm + offsets::Children);
        if (children == 0 || !IsValidPointer(children)) {
            LOG_WARN("[Privilege]   validation: DataModel Children pointer 0x%llX invalid — candidate rejected",
                     (unsigned long long)children);
            return 0;
        }
    } catch (...) {
        LOG_WARN("[Privilege]   validation: could not read DataModel Children — candidate rejected");
        return 0;
    }

    // Validate ClassName via ClassDescriptor chain
    std::string className = ReadClassName(dm);
    LOG_INFO("[Privilege]   validation: ClassName = '%s'", className.c_str());
    if (className.empty() || className.find("DataModel") == std::string::npos) {
        LOG_WARN("[Privilege]   validation: ClassName '%s' does not match DataModel — candidate rejected",
                 className.c_str());
        return 0;
    }

    // Probe for proxy detection: if ScriptContext is null AND children are empty,
    // this is likely a shell/proxy DataModel — warn but still return it
    try {
        uintptr_t sc = Memory::Read<uintptr_t>(dm + offsets::ScriptContext);
        uintptr_t childStart = Memory::Read<uintptr_t>(dm + offsets::Children);
        uintptr_t childEnd   = Memory::Read<uintptr_t>(dm + offsets::Children + offsets::ChildrenEnd);
        if (sc == 0 && childEnd <= childStart) {
            LOG_WARN("[Privilege]   Path A: candidate is likely a proxy "
                     "(ScriptContext=null, children empty) — will try other paths");
        }
    } catch (...) {}

    LOG_INFO("[Privilege]   Path A: DataModel validated @ 0x%llX", (unsigned long long)dm);
    return dm;
}

static uintptr_t ResolveDataModelPathB(uintptr_t moduleBase) {
    LOG_INFO("[Privilege] Path B: TaskScheduler/RenderJob chain");

    uintptr_t tsPtrAddr = moduleBase + offsets::TaskSchedulerPointer;
    LOG_INFO("[Privilege]   step 1: reading TaskScheduler [moduleBase+0x%llX] = [0x%llX]",
             (unsigned long long)offsets::TaskSchedulerPointer,
             (unsigned long long)tsPtrAddr);

    uintptr_t tsPtr = 0;
    try {
        tsPtr = Memory::Read<uintptr_t>(tsPtrAddr);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   step 1 FAILED: %s", e.what());
        return 0;
    }

    if (tsPtr == 0 || !IsValidPointer(tsPtr)) {
        LOG_WARN("[Privilege]   step 1: TaskScheduler 0x%llX invalid", (unsigned long long)tsPtr);
        return 0;
    }
    LOG_INFO("[Privilege]   step 1 result: TaskScheduler = 0x%llX", (unsigned long long)tsPtr);

    // Read job list start/end
    uintptr_t jobStart = 0, jobEnd = 0;
    try {
        jobStart = Memory::Read<uintptr_t>(tsPtr + offsets::JobStart);
        jobEnd   = Memory::Read<uintptr_t>(tsPtr + offsets::JobEnd);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   job list read FAILED: %s", e.what());
        return 0;
    }

    if (!IsValidPointer(jobStart) || !IsValidPointer(jobEnd) || jobEnd <= jobStart) {
        LOG_WARN("[Privilege]   job list invalid: start=0x%llX end=0x%llX",
                 (unsigned long long)jobStart, (unsigned long long)jobEnd);
        return 0;
    }

    size_t maxJobs = 256;
    size_t count = 0;
    size_t readable = 0;
    for (uintptr_t cur = jobStart; cur < jobEnd && count < maxJobs; cur += sizeof(uintptr_t), ++count) {
        try {
            uintptr_t jobPtr = Memory::Read<uintptr_t>(cur);
            if (!IsValidPointer(jobPtr)) continue;

            std::string jobName = ReadMSVCString(jobPtr + offsets::Job_Name);
            if (!jobName.empty()) ++readable;

            LOG_INFO("[Privilege]   job #%zu @ 0x%llX name='%s'",
                     count, (unsigned long long)jobPtr, jobName.c_str());

            std::string lower = jobName;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("render") != std::string::npos) {
                LOG_INFO("[Privilege]   found RenderJob '%s' @ 0x%llX", jobName.c_str(), (unsigned long long)jobPtr);

                uintptr_t dm = Memory::Read<uintptr_t>(jobPtr + offsets::RenderJobRealDataModel);
                if (dm != 0 && IsValidPointer(dm)) {
                    LOG_INFO("[Privilege]   Path B: DataModel = 0x%llX", (unsigned long long)dm);
                    return dm;
                }
                LOG_WARN("[Privilege]   RenderJob DataModel 0x%llX invalid", (unsigned long long)dm);
            }
        } catch (...) { continue; }
    }

    LOG_WARN("[Privilege]   Path B: no valid RenderJob found in %zu entries (%zu readable names)",
             count, readable);
    return 0;
}

uintptr_t ResolveDataModelPathC(uintptr_t moduleBase) {
    LOG_INFO("[Privilege] Path C: VisualEngine chain");

    uintptr_t vePtrAddr = moduleBase + offsets::VisualEnginePointer;
    uintptr_t veContainer = 0;
    try {
        veContainer = Memory::Read<uintptr_t>(vePtrAddr);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   step 1 FAILED: %s", e.what());
        return 0;
    }

    if (veContainer == 0 || !IsValidPointer(veContainer)) {
        LOG_WARN("[Privilege]   step 1: VisualEngine container 0x%llX invalid",
                 (unsigned long long)veContainer);
        return 0;
    }
    LOG_INFO("[Privilege]   step 1: VisualEngine container = 0x%llX",
             (unsigned long long)veContainer);

    uintptr_t veObj = 0;
    try {
        veObj = Memory::Read<uintptr_t>(veContainer + offsets::VisualEngine);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   step 2 FAILED: %s", e.what());
        return 0;
    }

    if (veObj == 0 || !IsValidPointer(veObj)) {
        LOG_WARN("[Privilege]   step 2: VisualEngine object 0x%llX invalid",
                 (unsigned long long)veObj);
        return 0;
    }
    LOG_INFO("[Privilege]   step 2: VisualEngine object = 0x%llX", (unsigned long long)veObj);

    // Probe known offsets for DataModel-like pointers
    const uintptr_t probeOffsets[] = { 0x1C8, 0x1D0, 0x38, 0x28, 0x30, 0x40, 0x48 };
    for (uintptr_t off : probeOffsets) {
        try {
            uintptr_t candidate = Memory::Read<uintptr_t>(veObj + off);
            if (candidate == 0 || !IsValidPointer(candidate)) continue;

            std::string cn = ReadClassName(candidate);
            if (!cn.empty() && cn.find("DataModel") != std::string::npos) {
                LOG_INFO("[Privilege]   Path C: found DataModel @ 0x%llX via VisualEngine+0x%llX (class='%s')",
                         (unsigned long long)candidate, (unsigned long long)off, cn.c_str());
                return candidate;
            }
        } catch (...) { continue; }
    }

    LOG_WARN("[Privilege]   Path C: no DataModel found via VisualEngine probing");
    return 0;
}

uintptr_t ResolveDataModel(uintptr_t moduleBase, std::string& pathUsed) {
    uintptr_t dm = ResolveDataModelPathA(moduleBase);
    if (dm != 0) { pathUsed = "PathA"; return dm; }

    dm = ResolveDataModelPathB(moduleBase);
    if (dm != 0) { pathUsed = "PathB"; return dm; }

    dm = ResolveDataModelPathC(moduleBase);
    if (dm != 0) { pathUsed = "PathC"; return dm; }

    pathUsed = "none";
    return 0;
}

// ============================================================
//  ScriptContext Fallback via Children Walk
// ============================================================

bool ResolveScriptContextViaChildren(uintptr_t dataModel, uintptr_t& outScriptContext) {
    LOG_INFO("[Privilege] Children walk: scanning DataModel children for ScriptContext...");

    try {
        uintptr_t childPtr = Memory::Read<uintptr_t>(dataModel + offsets::Children);
        uintptr_t childEnd = Memory::Read<uintptr_t>(dataModel + offsets::Children + offsets::ChildrenEnd);

        LOG_INFO("[Privilege]   Children start=0x%llX end=0x%llX",
                 (unsigned long long)childPtr, (unsigned long long)childEnd);

        if (!IsValidPointer(childPtr) || !IsValidPointer(childEnd)) {
            LOG_WARN("[Privilege]   Children pointers invalid: start=0x%llX end=0x%llX",
                     (unsigned long long)childPtr, (unsigned long long)childEnd);
            return false;
        }

        if (childEnd <= childPtr) {
            LOG_WARN("[Privilege]   Children list empty or reversed (delta=%lld)",
                     (long long)(childEnd - childPtr));
            return false;
        }

        size_t childCount = (childEnd - childPtr) / sizeof(uintptr_t);
        LOG_INFO("[Privilege]   Children array: %zu entries", childCount);

        size_t maxChildren = 512;
        size_t scanned = 0;
        for (uintptr_t cur = childPtr; cur < childEnd && scanned < maxChildren;
             cur += sizeof(uintptr_t), ++scanned) {
            uintptr_t child = Memory::Read<uintptr_t>(cur);
            if (!IsValidPointer(child)) continue;

            std::string className = ReadClassName(child);
            if (className == "ScriptContext") {
                LOG_INFO("[Privilege]   FOUND ScriptContext @ 0x%llX (child #%zu)",
                         (unsigned long long)child, scanned);
                outScriptContext = child;
                return true;
            }
        }

        LOG_WARN("[Privilege]   ScriptContext not found among %zu children", scanned);
        return false;

    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   Children walk exception: %s", e.what());
        return false;
    }
}

// ============================================================
//  Pointer Chain Resolution (v2.1 — try all DataModel paths)
// ============================================================

static bool TryResolveScriptContext(uintptr_t dataModel, uintptr_t& outSC, std::string& pathSuffix) {
    outSC = 0;

    try {
        uintptr_t scDirect = Memory::Read<uintptr_t>(dataModel + offsets::ScriptContext);
        LOG_INFO("[Privilege] Direct ScriptContext [DataModel+0x%llX] = 0x%llX",
                 (unsigned long long)offsets::ScriptContext,
                 (unsigned long long)scDirect);
        if (scDirect != 0 && IsValidPointer(scDirect)) {
            outSC = scDirect;
            return true;
        }
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege] Direct ScriptContext read failed: %s", e.what());
    }

    LOG_INFO("[Privilege] Direct offset returned null — trying children walk fallback");
    uintptr_t scChild = 0;
    if (ResolveScriptContextViaChildren(dataModel, scChild)) {
        outSC = scChild;
        pathSuffix = "+ChildrenWalk";
        return true;
    }

    return false;
}

bool ResolveContext(ContextInfo& out) {
    out = ContextInfo{};

    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached()) {
        out.lastError = "Not attached to a process";
        return false;
    }

    out.attached   = true;
    out.moduleBase = engine.GetModuleBase();

    if (out.moduleBase == 0) {
        out.lastError = "Module base is 0";
        return false;
    }

    LOG_INFO("[Privilege] === Chain resolution START (moduleBase=0x%llX) ===",
             (unsigned long long)out.moduleBase);

    // Collect DataModel candidates from all paths (don't short-circuit)
    struct DMCandidate { uintptr_t addr; std::string path; };
    DMCandidate candidates[3];
    int nCandidates = 0;

    out.pathATried = true;
    uintptr_t dmA = ResolveDataModelPathA(out.moduleBase);
    if (dmA != 0) candidates[nCandidates++] = { dmA, "PathA" };

    out.pathBTried = true;
    uintptr_t dmB = ResolveDataModelPathB(out.moduleBase);
    if (dmB != 0) candidates[nCandidates++] = { dmB, "PathB" };

    out.pathCTried = true;
    uintptr_t dmC = ResolveDataModelPathC(out.moduleBase);
    if (dmC != 0) candidates[nCandidates++] = { dmC, "PathC" };

    out.candidateCount = nCandidates;

    if (nCandidates == 0) {
        out.lastError = "All DataModel resolution paths failed";
        LOG_ERROR("[Privilege] %s", out.lastError.c_str());
        return false;
    }

    LOG_INFO("[Privilege] %d DataModel candidate(s) found", nCandidates);

    // Try ScriptContext resolution on each candidate until one works
    for (int i = 0; i < nCandidates; ++i) {
        LOG_INFO("[Privilege] Trying DataModel candidate %d/%d: 0x%llX via %s",
                 i + 1, nCandidates,
                 (unsigned long long)candidates[i].addr, candidates[i].path.c_str());

        std::string suffix;
        uintptr_t sc = 0;
        if (TryResolveScriptContext(candidates[i].addr, sc, suffix)) {
            out.dataModel      = candidates[i].addr;
            out.scriptContext   = sc;
            out.resolutionPath  = candidates[i].path + suffix;

            LOG_INFO("[Privilege] ScriptContext resolved @ 0x%llX via %s",
                     (unsigned long long)sc, out.resolutionPath.c_str());

            try {
                out.currentLevel = Memory::Read<int>(sc + offsets::ScriptContextIdentityLevel);
                LOG_INFO("[Privilege] Current identity level: %d", out.currentLevel);
            } catch (const std::exception& e) {
                LOG_WARN("[Privilege] Could not read identity level: %s", e.what());
            }

            try {
                uint8_t bypassByte = Memory::Read<uint8_t>(sc + offsets::ScriptContextRequireBypass);
                out.requireBypass = (bypassByte != 0);
                LOG_INFO("[Privilege] RequireBypass: %s", out.requireBypass ? "true" : "false");
            } catch (const std::exception& e) {
                LOG_WARN("[Privilege] Could not read bypass flag: %s", e.what());
            }

            LOG_INFO("[Privilege] === Chain resolution SUCCESS (path=%s) ===", out.resolutionPath.c_str());
            return true;
        }

        LOG_WARN("[Privilege] ScriptContext not found via %s — trying next candidate",
                 candidates[i].path.c_str());
    }

    out.lastError = "ScriptContext not found on any DataModel candidate";
    LOG_ERROR("[Privilege] %s", out.lastError.c_str());
    return false;
}

// ============================================================
//  Diagnostics
// ============================================================

void RunDiagnostics() {
    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached()) {
        LOG_WARN("[Privilege] RunDiagnostics: not attached");
        return;
    }

    uintptr_t base = engine.GetModuleBase();
    LOG_INFO("[Privilege] ============ DIAGNOSTICS START (moduleBase=0x%llX) ============",
             (unsigned long long)base);

    // 1. FakeDataModel structure
    try {
        uintptr_t fakeDM = Memory::Read<uintptr_t>(base + offsets::FakeDataModelPointer);
        if (fakeDM != 0 && IsValidPointer(fakeDM)) {
            DumpHex(fakeDM, 256, "FakeDataModel");

            uintptr_t dm = Memory::Read<uintptr_t>(fakeDM + offsets::FakeDataModelToDataModel);
            if (dm != 0 && IsValidPointer(dm)) {
                DumpHex(dm, 256, "PathA DataModel candidate");
                std::string cn = ReadClassName(dm);
                LOG_INFO("[Privilege]   PathA DataModel ClassName = '%s'", cn.c_str());

                uintptr_t sc = Memory::Read<uintptr_t>(dm + offsets::ScriptContext);
                LOG_INFO("[Privilege]   PathA ScriptContext @ +0x440 = 0x%llX", (unsigned long long)sc);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   FakeDataModel diagnostic failed: %s", e.what());
    }

    // 2. TaskScheduler + first 20 jobs
    try {
        uintptr_t tsPtr = Memory::Read<uintptr_t>(base + offsets::TaskSchedulerPointer);
        if (tsPtr != 0 && IsValidPointer(tsPtr)) {
            DumpHex(tsPtr, 256, "TaskScheduler");

            uintptr_t jobStart = Memory::Read<uintptr_t>(tsPtr + offsets::JobStart);
            uintptr_t jobEnd   = Memory::Read<uintptr_t>(tsPtr + offsets::JobEnd);
            LOG_INFO("[Privilege]   Jobs: start=0x%llX end=0x%llX",
                     (unsigned long long)jobStart, (unsigned long long)jobEnd);

            if (IsValidPointer(jobStart) && IsValidPointer(jobEnd) && jobEnd > jobStart) {
                size_t maxDump = 20;
                size_t count = 0;
                for (uintptr_t cur = jobStart; cur < jobEnd && count < maxDump;
                     cur += sizeof(uintptr_t), ++count) {
                    uintptr_t jobPtr = Memory::Read<uintptr_t>(cur);
                    if (!IsValidPointer(jobPtr)) continue;

                    std::string name = ReadMSVCString(jobPtr + offsets::Job_Name);
                    std::string cn = ReadClassName(jobPtr);

                    // Show raw bytes at Job_Name offset for SSO diagnosis
                    try {
                        auto raw = Memory::ReadBytes(jobPtr + offsets::Job_Name, 16);
                        char rawHex[64] = {};
                        for (size_t i = 0; i < raw.size() && i < 16; ++i)
                            sprintf(rawHex + i * 3, "%02X ", raw[i]);
                        size_t cap = Memory::Read<size_t>(jobPtr + offsets::Job_Name + 0x18);
                        LOG_INFO("[Privilege]   job #%zu @ 0x%llX name='%s' class='%s' raw=[%s] cap=%zu(%s)",
                                 count, (unsigned long long)jobPtr,
                                 name.c_str(), cn.c_str(), rawHex,
                                 cap, (cap < 16) ? "SSO" : "HEAP");
                    } catch (...) {
                        LOG_INFO("[Privilege]   job #%zu @ 0x%llX name='%s' class='%s'",
                                 count, (unsigned long long)jobPtr, name.c_str(), cn.c_str());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   TaskScheduler diagnostic failed: %s", e.what());
    }

    // 3. VisualEngine
    try {
        uintptr_t veContainer = Memory::Read<uintptr_t>(base + offsets::VisualEnginePointer);
        if (veContainer != 0 && IsValidPointer(veContainer)) {
            uintptr_t veObj = Memory::Read<uintptr_t>(veContainer + offsets::VisualEngine);
            if (veObj != 0 && IsValidPointer(veObj)) {
                DumpHex(veObj, 256, "VisualEngine object");
            } else {
                LOG_INFO("[Privilege]   VisualEngine container=0x%llX, object=0x%llX (invalid)",
                         (unsigned long long)veContainer, (unsigned long long)veObj);
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege]   VisualEngine diagnostic failed: %s", e.what());
    }

    LOG_INFO("[Privilege] ============ DIAGNOSTICS END ============");
}

// ============================================================
//  Identity Read / Write (Strategies 1 + 2)
// ============================================================

int GetPrivilegeLevel() {
    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        return 0;
    }
    return ctx.currentLevel;
}

bool SetPrivilegeLevel(int level) {
    if (level < 1 || level > 10) {
        LOG_ERROR("[Privilege] SetPrivilegeLevel: invalid level %d (must be 1-10)", level);
        return false;
    }

    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        LOG_WARN("[Privilege] SetPrivilegeLevel: %s", ctx.lastError.c_str());
        return false;
    }

    try {
        Memory::Write<int>(ctx.scriptContext + offsets::ScriptContextIdentityLevel, level);

        int confirmed = Memory::Read<int>(ctx.scriptContext + offsets::ScriptContextIdentityLevel);
        if (confirmed != level) {
            LOG_ERROR("[Privilege] SetPrivilegeLevel: readback mismatch (wrote %d, read %d)", level, confirmed);
            return false;
        }

        LOG_INFO("[Privilege] ScriptContext identity set to level %d (confirmed)", level);

        try {
            uintptr_t activeScript = Memory::Read<uintptr_t>(ctx.scriptContext + offsets::ScriptContextActiveScript);
            if (activeScript != 0 && IsValidPointer(activeScript)) {
                Memory::Write<int>(activeScript + offsets::ScriptIdentityLevel, level);
                int scriptConfirmed = Memory::Read<int>(activeScript + offsets::ScriptIdentityLevel);
                LOG_INFO("[Privilege] Active Script identity set to level %d (confirmed: %d)", level, scriptConfirmed);
            }
        } catch (const std::exception& e) {
            LOG_WARN("[Privilege] Active Script identity write failed (non-fatal): %s", e.what());
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("[Privilege] SetPrivilegeLevel exception: %s", e.what());
        return false;
    }
}

// ============================================================
//  Security Check Bypass (Strategy 3)
// ============================================================

bool BypassSecurityChecks(bool enable) {
    if (offsets::ScriptContextRequireBypass == 0) {
        LOG_WARN("[Privilege] BypassSecurityChecks: offset is 0x0 (overlaps vtable) — skipping to avoid corruption");
        return false;
    }

    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        LOG_WARN("[Privilege] BypassSecurityChecks: %s", ctx.lastError.c_str());
        return false;
    }

    try {
        uint8_t value = enable ? 1 : 0;
        Memory::Write<uint8_t>(ctx.scriptContext + offsets::ScriptContextRequireBypass, value);

        uint8_t confirmed = Memory::Read<uint8_t>(ctx.scriptContext + offsets::ScriptContextRequireBypass);
        if (confirmed != value) {
            LOG_ERROR("[Privilege] BypassSecurityChecks: readback mismatch (wrote %u, read %u)", value, confirmed);
            return false;
        }

        LOG_INFO("[Privilege] Security check bypass %s (confirmed)", enable ? "ENABLED" : "DISABLED");
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("[Privilege] BypassSecurityChecks exception: %s", e.what());
        return false;
    }
}

// ============================================================
//  Identity Verification Detour (Strategy 4)
// ============================================================

bool InstallIdentityCheckDetour(int level) {
    if (s_detourActive) {
        LOG_WARN("[Privilege] Detour already installed — remove it first");
        return false;
    }

    if (level < 1 || level > 10) {
        LOG_ERROR("[Privilege] InstallIdentityCheckDetour: invalid level %d", level);
        return false;
    }

    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        LOG_WARN("[Privilege] InstallIdentityCheckDetour: %s", ctx.lastError.c_str());
        return false;
    }

    try {
        uintptr_t fnAddr = Memory::Read<uintptr_t>(ctx.scriptContext + offsets::ScriptContextIdentityCheckFn);
        if (fnAddr == 0 || !IsValidPointer(fnAddr)) {
            LOG_ERROR("[Privilege] Identity-check function pointer 0x%llX invalid — detour not possible",
                      (unsigned long long)fnAddr);
            return false;
        }

        LOG_INFO("[Privilege] Identity-check function @ 0x%llX", (unsigned long long)fnAddr);

        for (int i = 0; i < 6; ++i)
            s_originalBytes[i] = Memory::Read<uint8_t>(fnAddr + i);

        uint8_t patch[6] = {
            0xB8,
            (uint8_t)(level & 0xFF),
            (uint8_t)((level >> 8) & 0xFF),
            (uint8_t)((level >> 16) & 0xFF),
            (uint8_t)((level >> 24) & 0xFF),
            0xC3
        };

        for (int i = 0; i < 6; ++i)
            Memory::Write<uint8_t>(fnAddr + i, patch[i]);

        uint8_t verify[6] = {};
        for (int i = 0; i < 6; ++i)
            verify[i] = Memory::Read<uint8_t>(fnAddr + i);

        if (memcmp(verify, patch, 6) != 0) {
            LOG_ERROR("[Privilege] Detour readback verification FAILED — restoring originals");
            for (int i = 0; i < 6; ++i)
                Memory::Write<uint8_t>(fnAddr + i, s_originalBytes[i]);
            return false;
        }

        s_detourAddress = fnAddr;
        s_detourActive  = true;

        LOG_INFO("[Privilege] Identity-check detour INSTALLED @ 0x%llX (level %d)",
                 (unsigned long long)fnAddr, level);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("[Privilege] InstallIdentityCheckDetour exception: %s", e.what());
        return false;
    }
}

bool RemoveIdentityCheckDetour() {
    if (!s_detourActive)
        return true;

    if (s_detourAddress == 0) {
        s_detourActive = false;
        return true;
    }

    try {
        for (int i = 0; i < 6; ++i)
            Memory::Write<uint8_t>(s_detourAddress + i, s_originalBytes[i]);

        uint8_t verify[6] = {};
        for (int i = 0; i < 6; ++i)
            verify[i] = Memory::Read<uint8_t>(s_detourAddress + i);

        if (memcmp(verify, s_originalBytes, 6) != 0) {
            LOG_ERROR("[Privilege] Detour removal readback FAILED — target may be in inconsistent state");
            return false;
        }

        LOG_INFO("[Privilege] Identity-check detour REMOVED @ 0x%llX", (unsigned long long)s_detourAddress);

        s_detourAddress = 0;
        s_detourActive  = false;
        memset(s_originalBytes, 0, sizeof(s_originalBytes));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("[Privilege] RemoveIdentityCheckDetour exception: %s", e.what());
        return false;
    }
}

// ============================================================
//  Orchestrator
// ============================================================

int Elevate(int targetLevel, bool useDetour) {
    LOG_INFO("[Privilege] === Elevation pipeline START (target: level %d) ===", targetLevel);

    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        LOG_ERROR("[Privilege] Elevate: cannot resolve ScriptContext — %s", ctx.lastError.c_str());
        return 0;
    }

    LOG_INFO("[Privilege] Initial level: %d, bypass: %s, path: %s",
             ctx.currentLevel, ctx.requireBypass ? "on" : "off",
             ctx.resolutionPath.c_str());

    int resultLevel = ctx.currentLevel;

    if (SetPrivilegeLevel(targetLevel)) {
        resultLevel = targetLevel;
    } else {
        LOG_WARN("[Privilege] Direct identity write did not confirm — continuing pipeline");
    }

    if (BypassSecurityChecks(true)) {
        LOG_INFO("[Privilege] Security bypass enabled");
    } else {
        LOG_WARN("[Privilege] Security bypass failed — continuing pipeline");
    }

    if (useDetour) {
        if (InstallIdentityCheckDetour(targetLevel)) {
            LOG_INFO("[Privilege] Identity verification detour active");
        } else {
            LOG_WARN("[Privilege] Detour installation failed");
        }
    }

    int finalLevel = GetPrivilegeLevel();
    if (finalLevel >= 7) {
        LOG_INFO("[Privilege] === Elevation SUCCESS: level %d (was %d) ===", finalLevel, ctx.currentLevel);
    } else if (finalLevel > ctx.currentLevel) {
        LOG_INFO("[Privilege] === Elevation PARTIAL: level %d (was %d, target %d) ===",
                 finalLevel, ctx.currentLevel, targetLevel);
    } else {
        LOG_WARN("[Privilege] === Elevation FAILED: level unchanged at %d ===", finalLevel);
    }

    return finalLevel;
}

// ============================================================
//  Retry Wrapper
// ============================================================

int ElevateWithRetry(int targetLevel, bool useDetour, int maxRetries, int delayMs) {
    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        LOG_INFO("[Privilege] Elevation attempt %d/%d (target: %d)", attempt, maxRetries, targetLevel);

        int level = Elevate(targetLevel, useDetour);
        if (level >= targetLevel) {
            LOG_INFO("[Privilege] Achieved level %d on attempt %d", level, attempt);
            return level;
        }

        if (attempt < maxRetries) {
            LOG_INFO("[Privilege] Attempt %d got level %d — retrying in %dms...", attempt, level, delayMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }

    LOG_ERROR("[Privilege] All %d elevation attempts exhausted", maxRetries);
    return 0;
}

// ============================================================
//  Auto-Elevate (called from Engine after attach)
// ============================================================

void AutoElevateOnAttach() {
    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached())
        return;

    nlohmann::json userCfg    = LoadConfigFile("config/user_config.json");
    nlohmann::json defaultCfg = LoadConfigFile("config/default_config.json");

    auto getCfg = [&](const std::string& key, auto defaultVal) -> decltype(defaultVal) {
        if (userCfg.contains("privilege") && userCfg["privilege"].contains(key))
            return userCfg["privilege"][key].get<decltype(defaultVal)>();
        if (defaultCfg.contains("privilege") && defaultCfg["privilege"].contains(key))
            return defaultCfg["privilege"][key].get<decltype(defaultVal)>();
        return defaultVal;
    };

    bool autoElevate = getCfg("auto_elevate_on_attach", true);
    if (!autoElevate) {
        LOG_INFO("[Privilege] Auto-elevate disabled in config — skipping");
        return;
    }

    int  targetLevel = getCfg("target_level", 10);
    bool useDetour   = getCfg("use_detour", false);
    int  retryCount  = getCfg("retry_count", 5);
    int  retryDelay  = getCfg("retry_delay_ms", 1000);

    if (targetLevel < 1)  targetLevel = 1;
    if (targetLevel > 10) targetLevel = 10;
    if (retryCount < 1)   retryCount = 1;
    if (retryDelay < 100)  retryDelay = 100;

    LOG_INFO("[Privilege] Auto-elevating to level %d (detour: %s, retries: %d, delay: %dms)...",
             targetLevel, useDetour ? "on" : "off", retryCount, retryDelay);

    int result = ElevateWithRetry(targetLevel, useDetour, retryCount, retryDelay);
    LOG_INFO("[Privilege] Auto-elevate complete — effective level: %d", result);
}

// ============================================================
//  Cleanup
// ============================================================

void Cleanup() {
    if (s_detourActive) {
        LOG_INFO("[Privilege] Cleaning up active detour...");
        if (!RemoveIdentityCheckDetour()) {
            LOG_ERROR("[Privilege] Detour cleanup FAILED — target may have residual patch");
        }
    }
    s_detourAddress = 0;
    memset(s_originalBytes, 0, sizeof(s_originalBytes));
}

} // namespace Privilege
