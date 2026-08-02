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
#include <vector>

namespace Privilege {

// ---- Route G: Runtime capture via debugger breakpoint ----
//
// Diagnostic tool for identifying which vtable slot is the per-frame
// Step() function (called continuously) versus rarely-called accessors,
// and what register (if any) holds a live ScriptContext-derived pointer
// at that moment — used when static offset chasing (Paths A-F) can't
// find ScriptContext anywhere in the objects it reaches.
struct BreakpointHit {
    uintptr_t address  = 0;
    int       hitCount = 0;
    // Register snapshot from the most recent hit at this address.
    uint64_t rax = 0, rbx = 0, rcx = 0, rdx = 0, rsi = 0, rdi = 0, rbp = 0, rsp = 0;
    uint64_t r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0, r13 = 0, r14 = 0, r15 = 0;
};

// Attaches as the target's debugger, writes a single 0xCC byte at each
// address in `addresses`, waits up to durationMs total for breakpoint
// hits, restores all original bytes, detaches, and returns per-address
// hit counts + last register snapshot in `results` (same order as input).
// Any exception/event not caused by our own breakpoints is passed through
// untouched (DBG_EXCEPTION_NOT_HANDLED) so the target's own handling isn't
// disturbed. Returns false if the debugger attach itself failed.
bool CaptureBreakpointHits(unsigned long pid, const std::vector<uintptr_t>& addresses,
                           int durationMs, std::vector<BreakpointHit>& results);

// ---- Route H: full read-only heap scan for a ScriptContext-classed object ----
//
// Walks every committed, private, readable region of the target's address
// space directly (VirtualQueryEx/ReadProcessMemory) and tests every
// plausible pointer value found in it for a ClassDescriptor->ClassName of
// "ScriptContext". Makes no assumption about any global/offset chain being
// correct — read-only, no debugger attach, no code patching. Bounded by
// timeBudgetMs; returns false (not found or budget exceeded) or true with
// outScriptContext set.
bool ScanForScriptContext(uintptr_t& outScriptContext, int timeBudgetMs = 60000);

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

// ---- Route D: ScriptContext via TaskQueue (bypasses DataModel) ----

// (internal — called by ResolveContext)

// ---- Route E: Parent-chain walk cross-validation ----

// (internal — called by ResolveContext)

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
