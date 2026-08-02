<!-- test/phase2_integration_test.md -->
# Phase 2 Integration Test Procedure

## Prerequisites
- Windows 10+ x64 VM (offline, educational use)
- test.exe running with Luau
- UniversalHub.exe built with Phase 2 changes
- Capcom.sys and PayloadDLL.dll in the same directory as UniversalHub.exe
- Run UniversalHub as Administrator

## Test 1: Driver Loading
1. Launch `UniversalHub.exe --attach test.exe`
2. Check log output: `[Main] Capcom.sys loaded — kernel R/W available`
3. If driver fails: `[Main] Capcom driver not loaded — falling back to user-mode RPM/WPM`
   - Check: Capcom.sys in directory, Administrator run, driver signing (test mode)

## Test 2: Manual Map Injection
1. In UniversalHub console: `bootstrap PayloadDLL.dll`
2. Expected: DLL maps via kernel path, pipe connects, PONG received
3. Console should show pipe server initialization messages
4. Check `pipe_connected()` returns true

## Test 3: Script Execution via Pipe
1. Execute: `UniversalHub.exe --attach test.exe --run phase2_test_manual_map.lua`
2. Expected output: `ALL PHASE 2 TESTS PASSED`
3. If Test 1 fails: manual mapping + pipe handshake failed
4. If Test 2 fails: identity elevation failed (ScriptContext not found or write failed)
5. If Test 3 fails: Workspace not accessible (privilege level too low)

## Test 4: Memory R/W Routing
1. With Capcom loaded, execute a script that uses `memory_read`/`memory_write`
2. Verify reads return same values as without Capcom
3. Check logs show no ReadProcessMemory calls (all via DeviceIoControl)

## Test 5: Error Handling
1. Intentionally fail Capcom loading (delete Capcom.sys)
2. Verify fallback to RPM/WPM works
3. Verify `pipe_execute` still works in fallback mode

## Test 6: Large Script (100KB+)
1. Test with a 100KB+ Lua script
2. Verify pipe protocol handles large payloads correctly
3. Verify no truncation or corruption

## Test 7: Shutdown + Cleanup
1. Exit UniversalHub
2. Check target process is still running normally (no crash)
3. Verify `Capcom` service is stopped and deleted (check `sc query Capcom`)
4. Check no leaked handles (Process Explorer)
