-- level_check.lua — Script Identity Elevation Diagnostic
-- Tests the full chain resolution and elevation pipeline.

log("info", "=== Level Check Diagnostic ===")

local ctx = resolve_context()
if not ctx then
    log("error", "resolve_context() returned nil — not attached?")
    return
end

log("info", "Attached:        " .. tostring(ctx.attached))
log("info", "Resolved:        " .. tostring(ctx.resolved))
log("info", "Module Base:     " .. string.format("0x%X", ctx.moduleBase))
log("info", "DataModel:       " .. string.format("0x%X", ctx.dataModel))
log("info", "ScriptContext:   " .. string.format("0x%X", ctx.scriptContext))
log("info", "Current Level:   " .. tostring(ctx.currentLevel))
log("info", "Resolution Path: " .. tostring(ctx.resolutionPath))
log("info", "Require Bypass:  " .. tostring(ctx.requireBypass))

if ctx.error then
    log("error", "Resolution error: " .. ctx.error)
end

if not ctx.resolved then
    log("warn", "Chain not resolved — cannot test elevation")
    return
end

log("info", "--- Attempting elevation to level 10 ---")

local ok, confirmed = set_privilege_level(10)
if ok then
    log("info", "set_privilege_level(10) succeeded, confirmed: " .. tostring(confirmed))
else
    log("warn", "set_privilege_level(10) failed: " .. tostring(confirmed))
end

local level = get_privilege_level()
log("info", "Current level after elevation: " .. tostring(level))

if level >= 10 then
    log("info", "SUCCESS: Level 10 — full core system access granted")
elseif level >= 7 then
    log("info", "PARTIAL: Level " .. level .. " — elevated but below target 10")
else
    log("error", "FAILED: Level " .. level .. " — elevation did not take effect")
end

log("info", "=== Diagnostic Complete ===")
