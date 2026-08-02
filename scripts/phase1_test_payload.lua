-- phase1_test_payload.lua — Phase 1 Integration Test Payload
-- ============================================================
-- Exercises the target's internal API surface to validate that:
--   1. The pipe protocol is functional (script received, result returned)
--   2. Privilege elevation succeeded (level >= 10)
--   3. Service access is working (DataModel, Workspace available)
--
-- Run via: pipe_execute_file("scripts/phase1_test_payload.lua")
-- Expected: success=true, result="OK: phase1 integration test passed"
--
-- FOR EDUCATIONAL DEMONSTRATION ONLY — controlled offline environment.

local testsRun = 0
local testsPassed = 0
local testsFailed = 0
local failures = {}

-- ---- Test Result Helpers ----

local function record(pass, name, detail)
    testsRun = testsRun + 1
    if pass then
        testsPassed = testsPassed + 1
    else
        testsFailed = testsFailed + 1
        table.insert(failures, {name = name, detail = detail})
    end
end

local function banner(text)
    local line = string.rep("-", 60)
    return line .. "\n  " .. text .. "\n" .. line
end

-- ---- Test 1: DataModel Access ----

local function test_datamodel()
    local ok, dm = pcall(get_service, "DataModel")
    record(ok and dm ~= nil,
        "get_service(\"DataModel\")",
        "DataModel should be accessible. ok=" .. tostring(ok) ..
        ", dm=" .. tostring(dm))
end

-- ---- Test 2: Identity Level (Privilege Elevation Verification) ----

local function test_identity_level()
    local ok, level = pcall(get_identity)
    if not ok or level == nil then
        record(false,
            "get_identity()",
            "Failed to retrieve identity level: " .. tostring(level))
        return
    end

    local pass = level >= 10
    record(pass,
        "get_identity() >= 10",
        "Current identity level: " .. tostring(level) ..
        " (expected >= 10)")
end

-- ---- Test 3: Workspace Access ----

local function test_workspace()
    local ok, ws = pcall(get_service, "Workspace")
    record(ok and ws ~= nil,
        "get_service(\"Workspace\")",
        "Workspace should be accessible. ok=" .. tostring(ok) ..
        ", ws=" .. tostring(ws))
end

-- ---- Test 4: ScriptContext-Level Service Access ----

local function test_scriptcontext_services()
    -- These services require the elevated ScriptContext identity.
    -- Availability depends on the target's service registration.
    local services = {"Players", "Lighting", "ReplicatedStorage", "StarterGui"}

    for _, svcName in ipairs(services) do
        local ok, svc = pcall(get_service, svcName)
        local pass = ok and svc ~= nil
        record(pass,
            "get_service(\"" .. svcName .. "\")",
            "ok=" .. tostring(ok) .. ", svc=" .. tostring(svc))
    end
end

-- ---- Test 5: Return Value Round-Trip ----

local function test_return_value()
    -- A simple return should be captured and sent back as EXECUTE_RESULT
    -- This test is self-referential: if we reach here, the return value
    -- test is inherently passing because this script was invoked via pipe_execute.
    record(true,
        "Return value round-trip",
        "Script was successfully received, compiled, and executed via pipe")
end

-- ---- Main Test Runner ----

print(banner("Phase 1 Integration Test"))

test_datamodel()
test_identity_level()
test_workspace()
test_scriptcontext_services()
test_return_value()

-- ---- Summary ----

print(banner("Test Summary"))
print(string.format("  Tests run:    %d", testsRun))
print(string.format("  Passed:       %d", testsPassed))
print(string.format("  Failed:       %d", testsFailed))

if testsFailed > 0 then
    print("\n  Failures:")
    for _, f in ipairs(failures) do
        print(string.format("    [FAIL] %s", f.name))
        print(string.format("           %s", f.detail))
    end
    return string.format("FAIL: %d/%d tests passed", testsPassed, testsRun)
end

print("\n  All tests passed.")
return "OK: phase1 integration test passed"
