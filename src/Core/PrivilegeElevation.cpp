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
    for (uintptr_t cur = jobStart; cur < jobEnd && count < maxJobs; cur += sizeof(uintptr_t), ++count) {
        try {
            uintptr_t jobPtr = Memory::Read<uintptr_t>(cur);
            if (!IsValidPointer(jobPtr)) continue;

            std::string jobName;
            try {
                uintptr_t namePtr = Memory::Read<uintptr_t>(jobPtr + offsets::Job_Name);
                if (IsValidPointer(namePtr))
                    jobName = Memory::ReadString(namePtr, 64);
            } catch (...) { continue; }

            if (jobName.find("Render") != std::string::npos ||
                jobName.find("render") != std::string::npos) {
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

    LOG_WARN("[Privilege]   Path B: no valid RenderJob found in %zu entries", count);
    return 0;
}

uintptr_t ResolveDataModel(uintptr_t moduleBase, std::string& pathUsed) {
    // Path A: FakeDataModel chain (primary)
    uintptr_t dm = ResolveDataModelPathA(moduleBase);
    if (dm != 0) {
        pathUsed = "PathA";
        return dm;
    }

    // Path B: TaskScheduler/RenderJob (fallback)
    dm = ResolveDataModelPathB(moduleBase);
    if (dm != 0) {
        pathUsed = "PathB";
        return dm;
    }

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

        if (!IsValidPointer(childPtr) || !IsValidPointer(childEnd)) {
            LOG_WARN("[Privilege]   Children pointers invalid: start=0x%llX end=0x%llX",
                     (unsigned long long)childPtr, (unsigned long long)childEnd);
            return false;
        }

        if (childEnd <= childPtr) {
            LOG_WARN("[Privilege]   Children list empty or reversed");
            return false;
        }

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
//  Pointer Chain Resolution (v2)
// ============================================================

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

    // Step 1: Resolve DataModel via multi-path
    out.dataModel = ResolveDataModel(out.moduleBase, out.resolutionPath);
    if (out.dataModel == 0) {
        out.lastError = "All DataModel resolution paths failed";
        LOG_ERROR("[Privilege] %s", out.lastError.c_str());
        return false;
    }
    LOG_INFO("[Privilege] DataModel resolved @ 0x%llX via %s",
             (unsigned long long)out.dataModel, out.resolutionPath.c_str());

    // Step 2: Resolve ScriptContext (direct offset, then children walk)
    try {
        uintptr_t scDirect = Memory::Read<uintptr_t>(out.dataModel + offsets::ScriptContext);
        LOG_INFO("[Privilege] Direct ScriptContext [DataModel+0x%llX] = 0x%llX",
                 (unsigned long long)offsets::ScriptContext,
                 (unsigned long long)scDirect);

        if (scDirect != 0 && IsValidPointer(scDirect)) {
            out.scriptContext = scDirect;
        }
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege] Direct ScriptContext read failed: %s", e.what());
    }

    if (out.scriptContext == 0) {
        LOG_INFO("[Privilege] Direct offset returned null — trying children walk fallback");
        uintptr_t scChild = 0;
        if (ResolveScriptContextViaChildren(out.dataModel, scChild)) {
            out.scriptContext = scChild;
            out.resolutionPath += "+ChildrenWalk";
        }
    }

    if (out.scriptContext == 0) {
        out.lastError = "ScriptContext not found (direct offset null, children walk failed)";
        LOG_ERROR("[Privilege] %s", out.lastError.c_str());
        return false;
    }

    LOG_INFO("[Privilege] ScriptContext resolved @ 0x%llX", (unsigned long long)out.scriptContext);

    // Step 3: Read current identity level and bypass flag
    try {
        out.currentLevel = Memory::Read<int>(out.scriptContext + offsets::ScriptContextIdentityLevel);
        LOG_INFO("[Privilege] Current identity level: %d", out.currentLevel);
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege] Could not read identity level: %s", e.what());
    }

    try {
        uint8_t bypassByte = Memory::Read<uint8_t>(out.scriptContext + offsets::ScriptContextRequireBypass);
        out.requireBypass = (bypassByte != 0);
        LOG_INFO("[Privilege] RequireBypass: %s", out.requireBypass ? "true" : "false");
    } catch (const std::exception& e) {
        LOG_WARN("[Privilege] Could not read bypass flag: %s", e.what());
    }

    LOG_INFO("[Privilege] === Chain resolution SUCCESS (path=%s) ===", out.resolutionPath.c_str());
    return true;
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
