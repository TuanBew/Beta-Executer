/**
 * PrivilegeElevation.h — Script Identity Elevation Module (v2)
 *
 * Manipulates the target process's internal script execution identity tier
 * to grant our injected scripts unrestricted access (level 7–10, matching
 * the authority of core system scripts).
 *
 * Resolution strategy (v2):
 *   Multi-path DataModel resolution with pointer validation at every step,
 *   ScriptContext fallback via children tree walk, and configurable retry.
 *
 * Elevation strategies (unchanged):
 *   Strategy 1+2 — Direct Identity Write / Spoof ScriptContext
 *   Strategy 3   — Bypass Security Checks (RequireBypass flag)
 *   Strategy 4   — Detour Identity Verification (function prologue patch)
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — controlled offline environment.
 */

#pragma once

#include <cstdint>
#include <string>

namespace Privilege {

struct ContextInfo {
    bool      attached         = false;
    uintptr_t moduleBase       = 0;
    uintptr_t fakeDataModel    = 0;
    uintptr_t dataModel        = 0;
    uintptr_t scriptContext    = 0;
    int       currentLevel     = 0;
    bool      requireBypass    = false;
    bool      detourInstalled  = false;
    std::string resolutionPath;
    std::string lastError;
    int         candidateCount   = 0;
    bool        pathATried       = false;
    bool        pathBTried       = false;
    bool        pathCTried       = false;
};

// ---- Pointer Chain Resolution (v2: multi-path + validation) ----

bool ResolveContext(ContextInfo& out);

// ---- Multi-Path DataModel Resolution ----

uintptr_t ResolveDataModel(uintptr_t moduleBase, std::string& pathUsed);

// ---- ScriptContext Fallback via Children Walk ----

bool ResolveScriptContextViaChildren(uintptr_t dataModel, uintptr_t& outScriptContext);

// ---- Identity Read / Write (Strategies 1 + 2) ----

int  GetPrivilegeLevel();
bool SetPrivilegeLevel(int level);

// ---- Security Check Bypass (Strategy 3) ----

bool BypassSecurityChecks(bool enable = true);

// ---- Identity Verification Detour (Strategy 4) ----

bool InstallIdentityCheckDetour(int level);
bool RemoveIdentityCheckDetour();

// ---- Path C: VisualEngine Resolution ----

uintptr_t ResolveDataModelPathC(uintptr_t moduleBase);

// ---- Diagnostics ----

void RunDiagnostics();

// ---- Orchestrator ----

int Elevate(int targetLevel = 10, bool useDetour = false);

int ElevateWithRetry(int targetLevel = 10, bool useDetour = false,
                     int maxRetries = 5, int delayMs = 1000);

// ---- Lifecycle ----

void AutoElevateOnAttach();
void Cleanup();

} // namespace Privilege
