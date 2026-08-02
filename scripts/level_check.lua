-- level_check.lua — Script Identity Elevation Diagnostic v3.0
-- Tests chain resolution with Routes A-E, job enumeration, diagnostics,
-- and elevation pipeline with hex dump verification.

log("info", "=== Level Check Diagnostic v3.0 ===")

-- Phase 1: Run full memory diagnostics (includes Route D probe)
log("info", "--- Running memory structure diagnostics ---")
run_diagnostics()

-- Phase 2: Enumerate TaskScheduler jobs
log("info", "--- Enumerating TaskScheduler jobs ---")
local jobs = enumerate_jobs()
if jobs then
    log("info", "Found " .. #jobs .. " job(s):")
    local scJob = nil
    for i, job in ipairs(jobs) do
        local addr = string.format("0x%X", job.address or 0)
        local line = "  #" .. i .. " addr=" .. addr ..
                     " name='" .. (job.name or "") ..
                     "' class='" .. (job.className or "") .. "'"
        log("info", line)

        local lower = string.lower(job.name or "")
        if string.find(lower, "scriptcontext") then
            scJob = job
        end
    end

    if scJob then
        log("info", "--- ScriptContextTaskQueue found: " ..
            string.format("0x%X", scJob.address) .. " ---")
        local hex = dump_memory(scJob.address, 256)
        if hex then
            log("info", "ScriptContextTaskQueue hex dump:")
            log("info", hex)
        end
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

-- Phase 3.5: Dump ScriptContext structure if resolved
if ctx.resolved and ctx.scriptContext ~= 0 then
    log("info", "--- ScriptContext hex dump (first 256 bytes) ---")
    local scDump = dump_memory(ctx.scriptContext, 256)
    if scDump then log("info", scDump) end

    -- Dump identity-relevant offsets
    local idLevel = read_memory(ctx.scriptContext + 0x2C0, "i32")
    local activeScript = read_memory(ctx.scriptContext + 0x2B0, "i64")
    local parentPtr = read_memory(ctx.scriptContext + 0x68, "i64")
    log("info", string.format("  SC+0x2C0 (identity): %s", tostring(idLevel)))
    log("info", string.format("  SC+0x2B0 (activeScript): 0x%X", activeScript or 0))
    log("info", string.format("  SC+0x68 (parent): 0x%X", parentPtr or 0))

    if parentPtr and parentPtr ~= 0 then
        local parentClass = get_class_name(parentPtr)
        log("info", "  Parent ClassName: " .. tostring(parentClass))
    end
end

-- Phase 3.6: Validate DataModel children array
if ctx.resolved and ctx.dataModel ~= 0 then
    log("info", "--- DataModel Children array validation ---")
    local childStart = read_memory(ctx.dataModel + 0x70, "i64")
    local childEnd = read_memory(ctx.dataModel + 0x78, "i64")
    log("info", string.format("  Children start: 0x%X", childStart or 0))
    log("info", string.format("  Children end:   0x%X", childEnd or 0))
    if childStart and childEnd and childEnd > childStart then
        local count = (childEnd - childStart) / 8
        log("info", string.format("  Children count: %d", count))

        -- Read first few children
        local maxShow = 10
        if count < maxShow then maxShow = count end
        for i = 0, maxShow - 1 do
            local childAddr = read_memory(childStart + i * 8, "i64")
            if childAddr and childAddr ~= 0 then
                local cn = get_class_name(childAddr)
                log("info", string.format("    child[%d] = 0x%X class='%s'", i, childAddr, cn or ""))
            end
        end
    else
        log("warn", "  Children array invalid or empty")
    end
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

-- Phase 5: Re-check context
local ctx2 = resolve_context()
if ctx2 and ctx2.resolved then
    log("info", "Post-elevation path: " .. tostring(ctx2.resolutionPath))
    log("info", "Post-elevation level: " .. tostring(ctx2.currentLevel))
end

-- Phase 6: Verdict
if level >= 10 then
    log("info", "SUCCESS: Level 10 achieved via " .. tostring(ctx.resolutionPath))
elseif level >= 7 then
    log("info", "PARTIAL: Level " .. level .. " — elevated but below target 10")
else
    log("error", "FAILED: Level " .. level .. " — elevation did not take effect")
    log("info", "Run scripts/research_dump.lua for detailed hex dumps")
end

log("info", "=== Diagnostic Complete ===")
