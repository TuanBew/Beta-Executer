/**
 * PrivilegeElevation.cpp — Script Identity Elevation Implementation
 *
 * See PrivilegeElevation.h for the architecture overview and strategy details.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — controlled offline environment.
 */

#include "Core/PrivilegeElevation.h"
#include "Core/Memory.h"
#include "Core/offsets.h"
#include "Logging/Logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace Privilege {

// ---- Module-level state ----

// Saved original bytes for Strategy 4 detour restoration
static uintptr_t s_detourAddress = 0;
static uint8_t  s_originalBytes[6] = {};
static bool     s_detourActive = false;

// ---- Helpers ----

/**
 * Read a JSON config file, falling back to an empty object on any error.
 */
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

// ============================================================
//  Pointer Chain Resolution
// ============================================================

bool ResolveContext(ContextInfo& out) {
    out = ContextInfo{}; // reset

    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached()) {
        out.lastError = "Not attached to a process";
        return false;
    }

    out.attached    = true;
    out.moduleBase  = engine.GetModuleBase();

    if (out.moduleBase == 0) {
        out.lastError = "Module base is 0";
        return false;
    }

    try {
        // Step 1: FakeDataModel pointer (absolute address in target)
        uintptr_t fakeDMPtrAddr = out.moduleBase + offsets::FakeDataModelPointer;
        out.fakeDataModel = Memory::Read<uintptr_t>(fakeDMPtrAddr);
        if (out.fakeDataModel == 0) {
            out.lastError = "FakeDataModel pointer is null (target may not be initialized)";
            return false;
        }
        LOG_DEBUG("[Privilege] FakeDataModel @ 0x%llX", (unsigned long long)out.fakeDataModel);

        // Step 2: DataModel via FakeDataModelToDataModel link
        out.dataModel = Memory::Read<uintptr_t>(out.fakeDataModel + offsets::FakeDataModelToDataModel);
        if (out.dataModel == 0) {
            out.lastError = "DataModel pointer is null (chain broken at FakeDataModel + 0x"
                          + std::to_string(offsets::FakeDataModelToDataModel) + ")";
            return false;
        }
        LOG_DEBUG("[Privilege] DataModel @ 0x%llX", (unsigned long long)out.dataModel);

        // Step 3: ScriptContext from DataModel
        out.scriptContext = Memory::Read<uintptr_t>(out.dataModel + offsets::ScriptContext);
        if (out.scriptContext == 0) {
            out.lastError = "ScriptContext pointer is null (chain broken at DataModel + ScriptContext)";
            return false;
        }
        LOG_DEBUG("[Privilege] ScriptContext @ 0x%llX", (unsigned long long)out.scriptContext);

        // Step 4: Read current identity level
        out.currentLevel = Memory::Read<int>(out.scriptContext + offsets::ScriptContextIdentityLevel);
        LOG_DEBUG("[Privilege] Current identity level: %d", out.currentLevel);

        // Step 5: Read bypass flag
        uint8_t bypassByte = Memory::Read<uint8_t>(out.scriptContext + offsets::ScriptContextRequireBypass);
        out.requireBypass = (bypassByte != 0);
        LOG_DEBUG("[Privilege] RequireBypass: %s", out.requireBypass ? "true" : "false");

        return true;

    } catch (const std::exception& e) {
        out.lastError = std::string("Chain resolution exception: ") + e.what();
        LOG_WARN("[Privilege] %s", out.lastError.c_str());
        return false;
    }
}

// ============================================================
//  Identity Read / Write (Strategies 1 + 2)
// ============================================================

int GetPrivilegeLevel() {
    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        LOG_WARN("[Privilege] GetPrivilegeLevel: %s", ctx.lastError.c_str());
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
        // Strategy 2: Write identity on the ScriptContext object
        Memory::Write<int>(ctx.scriptContext + offsets::ScriptContextIdentityLevel, level);

        // Readback verification
        int confirmed = Memory::Read<int>(ctx.scriptContext + offsets::ScriptContextIdentityLevel);
        if (confirmed != level) {
            LOG_ERROR("[Privilege] SetPrivilegeLevel: readback mismatch (wrote %d, read %d)", level, confirmed);
            return false;
        }

        LOG_INFO("[Privilege] ScriptContext identity set to level %d (confirmed)", level);

        // Strategy 1: Also try to write the active Script's identity (non-fatal if this fails)
        try {
            uintptr_t activeScript = Memory::Read<uintptr_t>(ctx.scriptContext + offsets::ScriptContextActiveScript);
            if (activeScript != 0) {
                Memory::Write<int>(activeScript + offsets::ScriptIdentityLevel, level);
                int scriptConfirmed = Memory::Read<int>(activeScript + offsets::ScriptIdentityLevel);
                LOG_INFO("[Privilege] Active Script identity set to level %d (confirmed: %d)", level, scriptConfirmed);
            } else {
                LOG_DEBUG("[Privilege] No active Script — skipping per-script identity write");
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

        // Readback verification
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
        // Read the function pointer from ScriptContext
        uintptr_t fnAddr = Memory::Read<uintptr_t>(ctx.scriptContext + offsets::ScriptContextIdentityCheckFn);
        if (fnAddr == 0) {
            LOG_ERROR("[Privilege] Identity-check function pointer is null — detour not possible");
            return false;
        }

        LOG_INFO("[Privilege] Identity-check function @ 0x%llX", (unsigned long long)fnAddr);

        // Save original 6 bytes
        for (int i = 0; i < 6; ++i) {
            s_originalBytes[i] = Memory::Read<uint8_t>(fnAddr + i);
        }

        // Write patch: mov eax, level; ret  (B8 <imm32> C3)
        // On x64 this sets EAX (zero-extending to RAX), which is the return value.
        uint8_t patch[6] = {
            0xB8,                                   // mov eax, imm32
            (uint8_t)(level & 0xFF),                // imm32 byte 0
            (uint8_t)((level >> 8) & 0xFF),         // imm32 byte 1
            (uint8_t)((level >> 16) & 0xFF),        // imm32 byte 2
            (uint8_t)((level >> 24) & 0xFF),        // imm32 byte 3
            0xC3                                    // ret
        };

        for (int i = 0; i < 6; ++i) {
            Memory::Write<uint8_t>(fnAddr + i, patch[i]);
        }

        // Readback verification of the patched immediate
        uint8_t verify[6] = {};
        for (int i = 0; i < 6; ++i) {
            verify[i] = Memory::Read<uint8_t>(fnAddr + i);
        }

        if (memcmp(verify, patch, 6) != 0) {
            LOG_ERROR("[Privilege] Detour readback verification FAILED — restoring originals");
            for (int i = 0; i < 6; ++i) {
                Memory::Write<uint8_t>(fnAddr + i, s_originalBytes[i]);
            }
            return false;
        }

        s_detourAddress = fnAddr;
        s_detourActive  = true;

        LOG_INFO("[Privilege] Identity-check detour INSTALLED @ 0x%llX (level %d)", (unsigned long long)fnAddr, level);
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("[Privilege] InstallIdentityCheckDetour exception: %s", e.what());
        return false;
    }
}

bool RemoveIdentityCheckDetour() {
    if (!s_detourActive) {
        return true; // nothing to do
    }

    if (s_detourAddress == 0) {
        s_detourActive = false;
        return true;
    }

    try {
        for (int i = 0; i < 6; ++i) {
            Memory::Write<uint8_t>(s_detourAddress + i, s_originalBytes[i]);
        }

        // Readback verification
        uint8_t verify[6] = {};
        for (int i = 0; i < 6; ++i) {
            verify[i] = Memory::Read<uint8_t>(s_detourAddress + i);
        }

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

    // Step 1: Resolve context
    ContextInfo ctx;
    if (!ResolveContext(ctx)) {
        LOG_ERROR("[Privilege] Elevate: cannot resolve ScriptContext — %s", ctx.lastError.c_str());
        return 0;
    }

    LOG_INFO("[Privilege] Initial level: %d, bypass: %s",
             ctx.currentLevel, ctx.requireBypass ? "on" : "off");

    int resultLevel = ctx.currentLevel;

    // Step 2: Write identity level (Strategies 1+2)
    if (SetPrivilegeLevel(targetLevel)) {
        resultLevel = targetLevel;
    } else {
        LOG_WARN("[Privilege] Direct identity write did not confirm — continuing pipeline");
    }

    // Step 3: Bypass security checks (Strategy 3)
    if (BypassSecurityChecks(true)) {
        LOG_INFO("[Privilege] Security bypass enabled");
    } else {
        LOG_WARN("[Privilege] Security bypass failed — continuing pipeline");
    }

    // Step 4: Optional detour (Strategy 4)
    if (useDetour) {
        if (InstallIdentityCheckDetour(targetLevel)) {
            LOG_INFO("[Privilege] Identity verification detour active");
        } else {
            LOG_WARN("[Privilege] Detour installation failed");
        }
    }

    // Final readback
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
//  Auto-Elevate (called from Engine after attach)
// ============================================================

void AutoElevateOnAttach() {
    auto& engine = Engine::GetInstance();
    if (!engine.IsAttached()) {
        return;
    }

    // Load config (user_config takes precedence, falls back to defaults)
    nlohmann::json userCfg   = LoadConfigFile("config/user_config.json");
    nlohmann::json defaultCfg = LoadConfigFile("config/default_config.json");

    // Merge: user values override defaults
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

    int  targetLevel = getCfg("target_level", 8);
    bool useDetour   = getCfg("use_detour", false);

    // Clamp
    if (targetLevel < 1)  targetLevel = 1;
    if (targetLevel > 10) targetLevel = 10;

    LOG_INFO("[Privilege] Auto-elevating to level %d (detour: %s)...", targetLevel, useDetour ? "on" : "off");

    int result = Elevate(targetLevel, useDetour);
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
