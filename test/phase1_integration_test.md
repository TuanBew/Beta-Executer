# Phase 1 Integration Test Procedure

**Date:** 2026-08-02
**Scope:** End-to-end validation of pipe protocol, lua_State capture, privilege elevation, and script execution.
**Target:** `test.exe` (controlled offline test process)
**Tool:** `UniversalHub.exe` (host-side console)

---

## Prerequisites

1. Built `PayloadDLL.dll` and `UniversalHub.exe` from `src/` (Release x64).
2. `test.exe` running with an active Luau VM and a Heartbeat scheduler.
3. `UniversalHub.exe` launched from the build output directory (so it can find `PayloadDLL.dll`).
4. Scripts directory contains `scripts/phase1_test_payload.lua`.

---

## Test 1: Pipe Connection

**What it validates:** The DLL connects to the host-side named pipe server after injection.

### Preconditions

- `test.exe` is running.
- `UniversalHub.exe` is launched in console mode.

### Steps

1. Attach to the target process:
   ```
   > attach test.exe
   ```
2. Inject the payload DLL:
   ```
   > inject PayloadDLL.dll
   ```
3. Wait for the pipe connection log line:
   ```
   [PipeServer] DLL connected on \\.\pipe\UniversalHub
   ```
4. Verify connection from the Lua bridge:
   ```lua
   > print(pipe_connected())
   true
   ```

### Expected Output

- Log shows "[PipeServer] DLL connected on \\.\pipe\UniversalHub"
- `pipe_connected()` returns `true`

### If This Fails

- Check that `test.exe` has the Luau DLL loaded and a running scheduler.
- Verify `PayloadDLL.dll` was built with the same architecture (x64).
- Check `DllMain` return value — if FALSE, the pipe client thread did not spawn.
- Verify no other instance is holding the pipe (only one client supported in Phase 1).

---

## Test 2: lua_State Capture (PING/PONG)

**What it validates:** The VEH handler captured `lua_State*`, the Heartbeat trampoline fired, and the Script Manager is running and responsive.

### Preconditions

- Test 1 passed (pipe connected).
- Wait up to 10 seconds after injection for the VEH to fire and the Heartbeat trampoline to inject the Script Manager.

### Steps

1. Send a PING from the host:
   ```lua
   > local flags = pipe_ping()
   > print(string.format("State flags: 0x%X", flags))
   State flags: 0x00000001
   ```
2. Verify the `STATE_READY` flag (bit 0) is set:
   ```lua
   > print(flags & 1 ~= 0)
   true
   ```

### Expected Output

- PING receives a PONG response within the pipe timeout (synchronous, should be immediate).
- State flags have bit 0 (`STATE_READY`) set.
- If `STATE_ERROR` (bit 2) is set, check the Script Manager initialization log.

### If This Fails

- **PING timeout / no PONG:** The Script Manager may not have started. Check:
  - Is the target actively calling `lua_pcall`? (Trigger any in-game action that runs Lua.)
  - Did `CaptureLuaState()` succeed? Check DebugView for VEH activity.
  - Did the Heartbeat trampoline fire? Check DebugView for `HeartbeatTrampolineCallback` calls.
- **PONG received but `STATE_ERROR`:** The Script Manager failed during initialization. Check the error payload in the PONG response (Task 6 wire format may include error string).

---

## Test 3: Script Execution (pipe_execute with Simple Return)

**What it validates:** A script is sent via EXECUTE_SCRIPT, compiled by the target's Luau VM, executed, and the result is returned.

### Preconditions

- Test 2 passed (PING/PONG working, Script Manager ready).

### Steps

1. Execute a simple script that returns a value:
   ```lua
   > local ok, err = pipe_execute("return 'hello from target pipeline'")
   > print(ok, err)
   true    nil
   ```
2. Verify the pipe is still connected:
   ```lua
   > print(pipe_connected())
   true
   ```

### Expected Output

- `ok` is `true`, `err` is `nil`.
- `pipe_connected()` still returns `true`.

### If This Fails

- **`ok` is `false`:** Check the error message for details. Possible causes:
  - The target's Luau VM rejected the bytecode (version mismatch).
  - The Script Manager encountered a pipe read error.
- **Script hangs:** The Script Manager may have crashed or the target's scheduler stalled. Check the target process state in Task Manager / Process Explorer.

---

## Test 4: Privilege Elevation

**What it validates:** The Route H heap scan found ScriptContext, wrote level 10 to `ScriptContext+0x2C0`, and the elevated identity is visible from Lua.

### Preconditions

- Test 2 passed (Script Manager running).
- `ElevatePrivilege()` was called during `InitPayload` (after lua_State capture).

### Steps

1. Query the current identity level:
   ```lua
   > local ok, err = pipe_execute("return get_identity()")
   > print(ok, err)
   true    nil
   ```
2. The return value is printed by `pipe_execute`; alternatively, use a script that reports the level:
   ```lua
   > pipe_execute([[
   >>   local level = get_identity()
   >>   print("Identity level: " .. tostring(level))
   >>   return "level=" .. tostring(level)
   >> ]])
   Identity level: 10
   true    nil
   ```

### Expected Output

- Identity level is >= 10.
- If the target starts at a higher level (e.g., plugin context already at 8), the result should still be >= 10 after elevation.

### If This Fails

- **Level is lower than 10:** The Route H scan may not have found ScriptContext. Check:
  - DebugView for `ElevatePrivilege` log messages.
  - Run `scripts/level_check.lua` from a direct-injection context to diagnose.
  - Verify the `ScriptContext+0x2C0` offset is correct for this target version.
- **`get_identity()` returns nil or errors:** The Lua API may differ — verify the target exposes `get_identity` in its global environment.

---

## Test 5: Full Test Payload Execution

**What it validates:** The complete test suite in `scripts/phase1_test_payload.lua` passes all assertions — DataModel access, identity level check, Workspace access, ScriptContext-level services, and return value round-trip.

### Preconditions

- Tests 1-4 passed.
- `scripts/phase1_test_payload.lua` exists in the working directory.

### Steps

1. Execute the test payload via the pipe:
   ```lua
   > local ok, err = pipe_execute_file("scripts/phase1_test_payload.lua")
   > print(ok, err)
   true    nil
   ```
2. Check the console output for the test summary banner:
   ```
   ------------------------------------------------------------
     Phase 1 Integration Test
   ------------------------------------------------------------
   ...
   ------------------------------------------------------------
     Test Summary
   ------------------------------------------------------------
     Tests run:    8
     Passed:       8
     Failed:       0

     All tests passed.
   ```

### Expected Output

- `ok` is `true`, indicating no compilation or runtime errors.
- All individual tests pass (Tests run == Passed, Failed == 0).
- Script returns `"OK: phase1 integration test passed"` as the execution result.

### If This Fails

- **Individual test failures:** The failure details are printed in the output. Common causes:
  - `get_service("DataModel")` nil: elevation may not have taken effect, or the target's service registry differs.
  - `get_identity() < 10`: elevation did not apply (see Test 4 debug steps).
  - `get_service("Workspace")` nil: DataModel resolved but the child lookup failed.
- **Script fails to compile:** The payload may use syntax not supported by the target's Luau version. Test with a minimal script first.

---

## Test 6: Error Handling

**What it validates:** Invalid Lua syntax is properly caught, reported as an error, and the pipe connection remains stable for subsequent commands.

### Preconditions

- Tests 2 passed (Script Manager running).

### Steps

1. Send a script with a syntax error:
   ```lua
   > local ok, err = pipe_execute("this is not valid lua syntax!!!!")
   > print("ok:", ok)
   ok: false
   > print("err:", err)
   err: Compile error: [string "=pipe_script"]:1: syntax error...
   ```
2. Send a script with a runtime error:
   ```lua
   > local ok, err = pipe_execute("return nil + 1")
   > print("ok:", ok)
   ok: false
   > print("err:", err)
   err: Runtime error: attempt to perform arithmetic on a nil value...
   ```
3. Verify the pipe is still functional after errors:
   ```lua
   > local ok, err = pipe_execute("return 'recovery test'")
   > print(ok, err)
   true    nil
   > print(pipe_connected())
   true
   ```

### Expected Output

- Syntax errors produce `ok=false` with error containing "Compile error".
- Runtime errors produce `ok=false` with error containing "Runtime error".
- The pipe remains connected and functional after each error.
- `pipe_connected()` returns `true` throughout.

### If This Fails

- **Error not reported (ok=true on invalid syntax):** The Script Manager may not be catching `loadstring` failures. Check the Lua-side error handling in `ScriptManager.lua`.
- **Pipe disconnects after error:** The Script Manager may be crashing. Check if the coroutine error handling properly catches exceptions without killing the heartbeat callback.

---

## Test 7: Large Script Transfer (~500 KB)

**What it validates:** The pipe protocol handles scripts up to at least 500 KB without truncation, buffer overflow, or frame boundary corruption.

### Preconditions

- Test 2 passed (Script Manager running).

### Steps

1. Generate a large script (the host-side `pipe_execute` handles this automatically; the script is a simple loop that returns a known value):
   ```lua
   > -- Build a ~500 KB script: a large comment block + a return statement
   > local prefix = "--" .. string.rep("X", 500 * 1024) .. "\nreturn 'large script OK'"
   > local ok, err = pipe_execute(prefix)
   > print(ok, err)
   true    nil
   ```

   Alternatively, create a file `scripts/large_test_payload.lua`:
   ```lua
   -- scripts/large_test_payload.lua
   -- ~500 KB comment block for transfer size validation
   ```
   And execute it:
   ```lua
   > pipe_execute_file("scripts/large_test_payload.lua")
   ```

2. Verify the script executed correctly:
   ```lua
   > local ok, err = pipe_execute("return 'post-large-script connectivity check'")
   > print(ok, err)
   true    nil
   ```

### Expected Output

- The large script is transmitted and executed without error.
- The pipe remains functional for subsequent commands.
- No "buffer overflow", "truncation", or "incomplete frame" errors in the DebugView log.

### If This Fails

- **Truncated script:** The 4-byte LE `PayloadLen` field may overflow. Check the frame serialization — max theoretical size is ~2 GB, but the pipe buffers are 64 KB each. The read loop in `ScriptManager.lua` must handle multi-read payloads.
- **Pipe disconnects during transfer:** The target may have a timeout on the Heartbeat callback. Large scripts that take too long to transfer may exceed this. Reduce the script size or check the `ReadFile` timeout on the pipe client side.
- **Script compiles but produces wrong results:** The payload may have been truncated mid-byte. Add a checksum or known suffix to the payload and verify it arrives intact.

---

## Cleanup

After all tests complete:

```
> detach
[PipeServer] Shutting down...
[Engine] Detached from test.exe
> exit
```

---

## Test Summary Checklist

| Test | Name | Status |
|------|------|--------|
| 1 | Pipe connection | [ ] |
| 2 | lua_State capture (PING/PONG) | [ ] |
| 3 | Script execution (pipe_execute) | [ ] |
| 4 | Privilege elevation | [ ] |
| 5 | Full test payload | [ ] |
| 6 | Error handling | [ ] |
| 7 | Large script (~500 KB) | [ ] |

---

## Notes

- All tests assume a **controlled offline environment** with a known test target (`test.exe`).
- The Phase 1 injection uses `CreateRemoteThread` + `LoadLibraryA` — the DLL must be accessible from the target's working directory or system path.
- DebugView (Sysinternals) or `OutputDebugString` is recommended for monitoring DLL-side log output during debugging.
- If the target process is restarted between tests, re-inject following the Test 1 procedure.
- Phase 1 is single-client; only one test session can run at a time per pipe name.
