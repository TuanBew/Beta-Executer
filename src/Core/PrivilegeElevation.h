/**
 * PrivilegeElevation.h — Script Identity Elevation Module
 *
 * Manipulates the target process's internal script execution identity tier
 * to grant our injected scripts unrestricted access (level 7–10, matching
 * the authority of core system scripts).
 *
 * Four strategies are implemented, applied in order of reliability:
 *
 *   Strategy 1+2 — Direct Identity Write / Spoof ScriptContext
 *     Walk the DataModel → ScriptContext pointer chain and write the
 *     identity level field. Readback-verified to confirm success.
 *
 *   Strategy 3 — Bypass Security Checks
 *     Toggle the ScriptContextRequireBypass flag to disable all
 *     identity verification entirely.
 *
 *   Strategy 4 — Detour Identity Verification
 *     Patch the identity-check function prologue in the target to
 *     always return the desired level. Restorable on detach.
 *
 * All memory operations are individually wrapped in try/catch — a failure
 * at any step is logged and the chain short-circuits gracefully. Never
 * throws out of the module boundary.
 *
 * FOR EDUCATIONAL DEMONSTRATION ONLY — controlled offline environment.
 */

#pragma once

#include <cstdint>
#include <string>

namespace Privilege {

/**
 * Holds the full resolved pointer chain for the target's script execution
 * context. Every field is zero-initialized; non-zero means successfully read.
 */
struct ContextInfo {
    bool     attached         = false;
    uintptr_t moduleBase      = 0;
    uintptr_t fakeDataModel   = 0;   // moduleBase + FakeDataModelPointer
    uintptr_t dataModel       = 0;   // fakeDM    + FakeDataModelToDataModel
    uintptr_t scriptContext   = 0;   // dataModel + ScriptContext
    int      currentLevel     = 0;   // ScriptContext + ScriptContextIdentityLevel
    bool     requireBypass    = false; // ScriptContext + ScriptContextRequireBypass
    bool     detourInstalled  = false;
    std::string lastError;
};

// ---- Pointer Chain Resolution ----

/**
 * Walk the full pointer chain from the target's main module base down to
 * the ScriptContext object. Fills every field of `out` that can be reached.
 *
 * @return true if the ScriptContext address resolved to a non-null value.
 */
bool ResolveContext(ContextInfo& out);

// ---- Identity Read / Write (Strategies 1 + 2) ----

/**
 * Read the current identity level from the target's ScriptContext.
 * Requires ResolveContext() to have succeeded first (called internally).
 *
 * @return Current level (0 = unresolved / not attached / error).
 */
int GetPrivilegeLevel();

/**
 * Write a new identity level into the target's ScriptContext and the
 * active Script object (if reachable). Readback-verified.
 *
 * @param level  Desired identity tier (1–10).
 * @return true if the write succeeded AND readback confirmed the level.
 */
bool SetPrivilegeLevel(int level);

// ---- Security Check Bypass (Strategy 3) ----

/**
 * Toggle the ScriptContextRequireBypass flag at offset 0x0 of the
 * ScriptContext object. When enabled, all identity checks are skipped.
 *
 * @param enable  true = bypass all checks, false = restore normal checks.
 * @return true if the write succeeded and readback confirmed.
 */
bool BypassSecurityChecks(bool enable = true);

// ---- Identity Verification Detour (Strategy 4) ----

/**
 * Patch the target's identity-verification function prologue to always
 * return the given level. Saves original bytes for later restoration.
 *
 * This is an in-place immediate patch: the function's first 6 bytes are
 * overwritten with `mov eax, imm32; ret` (B8 <level> C3 on x64).
 * Only installs if the target function can be read and the patch
 * readback-verifies.
 *
 * @param level  The level the patched function should always return.
 * @return true if the patch was written and readback-verified.
 */
bool InstallIdentityCheckDetour(int level);

/**
 * Restore the original bytes of the identity-verification function.
 * Must be called before detaching to leave the target clean.
 *
 * @return true if original bytes were restored and verified.
 */
bool RemoveIdentityCheckDetour();

// ---- Orchestrator ----

/**
 * Run the full elevation pipeline:
 *   1. ResolveContext()
 *   2. SetPrivilegeLevel(targetLevel)
 *   3. BypassSecurityChecks(true)
 *   4. (optional) InstallIdentityCheckDetour(targetLevel)
 *
 * Each step logs its result. The chain continues even if an early step
 * fails — later steps may still succeed independently.
 *
 * @param targetLevel  Desired identity tier (default 8).
 * @param useDetour    Whether to also install Strategy 4.
 * @return The final confirmed readback level, or 0 on total failure.
 */
int Elevate(int targetLevel = 8, bool useDetour = false);

/**
 * Called automatically after Engine::AttachToProcess() succeeds.
 * Reads the privilege config section (auto_elevate_on_attach,
 * target_level, bypass_security_checks, use_detour) and runs Elevate()
 * if auto-elevate is enabled.
 *
 * Gracefully handles missing config — falls back to defaults
 * (level 8, bypass on, detour off).
 */
void AutoElevateOnAttach();

/**
 * Restore any active detour bytes. Called from Engine::Detach().
 * Idempotent — safe to call even if no detour was installed.
 */
void Cleanup();

} // namespace Privilege
