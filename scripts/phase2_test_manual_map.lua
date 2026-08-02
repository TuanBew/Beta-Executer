-- scripts/phase2_test_manual_map.lua
-- Phase 2 integration test: verifies manual map injection + pipe communication
--
-- Prerequisites:
--   1. test.exe running with Luau linked
--   2. UniversalHub built with Phase 2 changes
--   3. Capcom.sys in output directory
--   4. PayloadDLL.dll in output directory
--
-- Test flow:
--   1. Load Capcom driver
--   2. Manual-map PayloadDLL
--   3. Verify pipe connection (ping/pong)
--   4. Execute a simple script through pipe
--   5. Verify DataModel access (confirms identity elevation worked)

print("=== Phase 2 Integration Test ===\n")

-- Test 1: Verify Capcom driver is loaded
-- (pipe_execute passes script to target; we check it runs)
local result, err = pipe_execute([[
    return "TEST_1_PASS: Script execution via manual map works"
]])
assert(result and string.find(result, "TEST_1_PASS"), "Test 1 failed: " .. (err or "nil"))

-- Test 2: Verify DataModel access (requires elevated identity)
local result, err = pipe_execute([[
    local dm = game:GetService("DataModel")
    return "TEST_2_PASS: DataModel=" .. tostring(dm)
]])
assert(result and string.find(result, "TEST_2_PASS"), "Test 2 failed: " .. (err or "nil"))

-- Test 3: Verify Workspace access
local result, err = pipe_execute([[
    local ws = game:GetService("Workspace")
    return "TEST_3_PASS: Workspace=" .. tostring(ws)
]])
assert(result and string.find(result, "TEST_3_PASS"), "Test 3 failed: " .. (err or "nil"))

-- Test 4: Verify round-trip with larger payload (1KB)
local payload = "return \"" .. string.rep("X", 800) .. "\""
local result, err = pipe_execute(payload)
assert(result and #result == 800, "Test 4 failed: expected 800 bytes, got " .. tostring(result and #result or 0))

print("\n=== ALL PHASE 2 TESTS PASSED ===")
return "OK: phase2 integration test passed"
