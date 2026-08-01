-- level_check.lua — Script Identity Elevation Diagnostic
-- FOR EDUCATIONAL DEMONSTRATION ONLY (controlled offline environment)
--
-- Verifies that the scripting runtime has been elevated to at least
-- identity tier 7 (core system script access).
--
-- Usage: Run from the Executer tab or via ExecuteFile in the GUI.

print("=== Script Identity Elevation Diagnostic ===")
print("")

-- Step 1: Read current identity level
local level = get_privilege_level()
if level then
    print("Current privilege level: " .. tostring(level))
else
    print("Current privilege level: (unresolved — target may not be attached)")
end

-- Step 2: Elevate if below target
if level and level < 7 then
    print("Below target tier (7+) — elevating to 8...")
    local ok, confirmed = set_privilege_level(8)
    if ok then
        print("Direct write confirmed at level " .. tostring(confirmed))
    else
        print("Direct write did not confirm: " .. tostring(confirmed))
    end
end

-- Step 3: Enable security bypass
local bypassOk = bypass_security_checks(true)
if bypassOk then
    print("Security check bypass: ENABLED")
else
    print("Security check bypass: FAILED — continuing")
end

-- Step 4: Final readback
local finalLevel = get_privilege_level()
print("")
print("--- Results ---")
print("Final privilege level: " .. tostring(finalLevel))

if finalLevel and finalLevel >= 7 then
    print("")
    print("=== RESULT: PASS (level " .. tostring(finalLevel) .. ") ===")
    print("Scripts now have core system script access.")
    print("Protected services, UI containers, and guarded memory regions are accessible.")
elseif finalLevel and finalLevel > 0 then
    print("")
    print("=== RESULT: PARTIAL (level " .. tostring(finalLevel) .. ") ===")
    print("Elevation partially succeeded but did not reach the target tier.")
else
    print("")
    print("=== RESULT: FAIL ===")
    print("The target process may not be attached or the ScriptContext chain is unresolved.")
    print("Verify that:")
    print("  1. The process is attached (check Injector tab)")
    print("  2. The target is the correct process with the expected memory layout")
    print("  3. The offsets in offsets.h match the target's internal structure")
end
