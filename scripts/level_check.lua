-- level_check.lua — Script Identity Elevation Diagnostic v2.1
-- Tests chain resolution, job enumeration, diagnostics, and elevation pipeline.

log("info", "=== Level Check Diagnostic v2.1 ===")

-- Phase 1: Run full memory diagnostics
log("info", "--- Running memory structure diagnostics ---")
run_diagnostics()

-- Phase 2: Enumerate TaskScheduler jobs
log("info", "--- Enumerating TaskScheduler jobs ---")
local jobs = enumerate_jobs()
if jobs then
    log("info", "Found " .. #jobs .. " job(s):")
    for i, job in ipairs(jobs) do
        local addr = string.format("0x%X", job.address or 0)
        log("info", "  #" .. i .. " addr=" .. addr .. " name='" .. (job.name or "") .. "' class='" .. (job.className or "") .. "'")
    end
else
    log("warn", "enumerate_jobs() returned nil — not attached?")
end

-- Phase 3: Resolve context
log("info", "--- Resolving pointer chain ---")
local ctx = resolve_context()
if not ctx then
    log("error", "resolve_context() returned nil — not attached?")
    return
end

log("info", "Attached:         " .. tostring(ctx.attached))
log("info", "Resolved:         " .. tostring(ctx.resolved))
log("info", "Module Base:      " .. string.format("0x%X", ctx.moduleBase))
log("info", "FakeDataModel:    " .. string.format("0x%X", ctx.fakeDataModel or 0))
log("info", "DataModel:        " .. string.format("0x%X", ctx.dataModel))
log("info", "ScriptContext:    " .. string.format("0x%X", ctx.scriptContext))
log("info", "Current Level:    " .. tostring(ctx.currentLevel))
log("info", "Resolution Path:  " .. tostring(ctx.resolutionPath))
log("info", "Require Bypass:   " .. tostring(ctx.requireBypass))
log("info", "Detour Installed: " .. tostring(ctx.detourInstalled))
log("info", "Candidates:       " .. tostring(ctx.candidateCount))
log("info", "Paths tried:      A=" .. tostring(ctx.pathATried) ..
            " B=" .. tostring(ctx.pathBTried) ..
            " C=" .. tostring(ctx.pathCTried))

if ctx.error then
    log("error", "Resolution error: " .. ctx.error)
end

if not ctx.resolved then
    log("warn", "Chain not resolved — cannot test elevation")
    return
end

-- Phase 4: Attempt elevation to level 10
log("info", "--- Attempting elevation to level 10 ---")

local ok, confirmed = set_privilege_level(10)
if ok then
    log("info", "set_privilege_level(10) succeeded, confirmed: " .. tostring(confirmed))
else
    log("warn", "set_privilege_level(10) failed: " .. tostring(confirmed))
end

local level = get_privilege_level()
log("info", "Current level after elevation: " .. tostring(level))

-- Phase 5: Re-check context to see which path succeeded
local ctx2 = resolve_context()
if ctx2 and ctx2.resolved then
    log("info", "Post-elevation path: " .. tostring(ctx2.resolutionPath))
    log("info", "Post-elevation level: " .. tostring(ctx2.currentLevel))
end

if level >= 10 then
    log("info", "SUCCESS: Level 10 — full core system access granted")
elseif level >= 7 then
    log("info", "PARTIAL: Level " .. level .. " — elevated but below target 10")
else
    log("error", "FAILED: Level " .. level .. " — elevation did not take effect")
end

log("info", "=== Diagnostic Complete ===")
