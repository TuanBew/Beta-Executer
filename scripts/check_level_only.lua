-- check_level_only.lua — resolve_context() only, no set_privilege_level() call.
-- Used to check whether a prior elevation write persists across a fresh
-- attach/detach cycle without re-applying it.

log("info", "=== Check Level Only ===")

local ctx = resolve_context()
if not ctx then
    log("error", "resolve_context() returned nil — not attached?")
    return
end

log("info", "Resolved:        " .. tostring(ctx.resolved))
log("info", "ScriptContext:   " .. string.format("0x%X", ctx.scriptContext or 0))
log("info", "Current Level:   " .. tostring(ctx.currentLevel))
log("info", "Resolution Path: " .. tostring(ctx.resolutionPath))

log("info", "=== Check Complete ===")
