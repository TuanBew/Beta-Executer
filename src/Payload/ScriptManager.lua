-- ScriptManager.lua — Embedded persistent execution layer
-- Injected into the target's Lua VM via one-shot Heartbeat trampoline.
-- Runs as legitimate Lua code within the target's scheduler.
--
-- FOR EDUCATIONAL DEMONSTRATION ONLY — controlled offline environment.

-- Pipe handle and protocol constants are set as globals by the C++
-- Heartbeat trampoline BEFORE this script is loaded.
local PIPE_READ_MODE = 0x02  -- PIPE_READMODE_MESSAGE
local FRAME_HEADER_SIZE = 11

-- Command constants (must match PipeProtocol.h)
local CMD_EXECUTE_SCRIPT = 0x01
local CMD_EXECUTE_RESULT = 0x02
local CMD_PING           = 0x03
local CMD_PONG           = 0x04
local CMD_SHUTDOWN       = 0x05

-- Result codes
local RESULT_OK    = 0
local RESULT_ERROR = 1
local RESULT_FATAL = 2

-- ---- Utility: read exactly N bytes from pipe ----
local function readBytes(pipeHandle, count)
    -- We use a Lua-exposed read function set by the C++ side.
    -- For Phase 1 testing, the C++ trampoline registers:
    --   pipe_read(hPipe, buffer, size) -> bytesRead
    -- into the global environment before loadstring(this script).
    local buf = ""
    local remaining = count
    while remaining > 0 do
        local chunk = pipe_read(pipeHandle, remaining)
        if not chunk or #chunk == 0 then
            return nil  -- pipe closed or error
        end
        buf = buf .. chunk
        remaining = remaining - #chunk
    end
    return buf
end

-- ---- Utility: write bytes to pipe ----
local function writeBytes(pipeHandle, data)
    return pipe_write(pipeHandle, data)
end

-- ---- Read a frame header ----
local function readFrameHeader(pipeHandle)
    local header = readBytes(pipeHandle, FRAME_HEADER_SIZE)
    if not header then return nil end

    -- Parse little-endian fields
    local magic = string.byte(header, 1)
        + string.byte(header, 2) * 256
        + string.byte(header, 3) * 65536
        + string.byte(header, 4) * 16777216
    if magic ~= 0x48554221 then  -- "HUB!"
        return nil  -- bad magic
    end

    local version = string.byte(header, 5)
    local cmd = string.byte(header, 6) + string.byte(header, 7) * 256
    local payloadLen = string.byte(header, 8)
        + string.byte(header, 9) * 256
        + string.byte(header, 10) * 65536
        + string.byte(header, 11) * 16777216

    return cmd, payloadLen
end

-- ---- Build and send a RESULT frame ----
local function sendResult(pipeHandle, status, errorMsg)
    local payload = string.char(status)
    if errorMsg then
        payload = payload .. errorMsg .. "\0"
    end
    local len = #payload

    -- Build frame header (11 bytes)
    local frame = string.char(0x21, 0x42, 0x55, 0x48)  -- "HUB!" LE
        .. string.char(0x01)       -- version
        .. string.char(CMD_EXECUTE_RESULT % 256)
        .. string.char(math.floor(CMD_EXECUTE_RESULT / 256))
        .. string.char(len % 256)
        .. string.char(math.floor(len / 256) % 256)
        .. string.char(math.floor(len / 65536) % 256)
        .. string.char(math.floor(len / 16777216))
        .. payload

    writeBytes(pipeHandle, frame)
end

-- ---- Build and send PONG frame with state flags ----
local function sendPong(pipeHandle, stateFlags)
    -- State flags packed as uint32 LE (4 bytes)
    local payload = string.char(stateFlags % 256)
        .. string.char(math.floor(stateFlags / 256) % 256)
        .. string.char(math.floor(stateFlags / 65536) % 256)
        .. string.char(math.floor(stateFlags / 16777216))
    local len = #payload

    local frame = string.char(0x21, 0x42, 0x55, 0x48)
        .. string.char(0x01)
        .. string.char(CMD_PONG % 256)
        .. string.char(math.floor(CMD_PONG / 256))
        .. string.char(len % 256)
        .. string.char(math.floor(len / 256) % 256)
        .. string.char(math.floor(len / 65536) % 256)
        .. string.char(math.floor(len / 16777216))
        .. payload

    writeBytes(pipeHandle, frame)
end

-- ---- Main Script Manager ----
-- Returns a function that takes a pipe handle argument.
-- The C++ side calls this returned function to start the manager.
return function(pipeHandle)
    local running = true
    local currentScript = nil
    local activeCoroutine = nil

    -- State flags for PONG responses
    local STATE_READY     = 1   -- bit 0
    local STATE_EXECUTING = 2   -- bit 1
    local STATE_ERROR     = 4   -- bit 2
    local stateFlags = STATE_READY

    -- Connect to the target's heartbeat scheduler.
    -- Uses the target's native task scheduler API.
    local success, scheduler = pcall(function()
        return get_scheduler and get_scheduler()
    end)
    if not success or not scheduler then
        -- Fallback: try alternative API surface
        sendResult(pipeHandle, RESULT_FATAL,
            "Script Manager: cannot access scheduler API")
        return
    end

    -- Register heartbeat callback.
    -- On each scheduler tick, poll the pipe and dispatch any pending commands.
    scheduler.Heartbeat:Connect(function()
        -- Non-blocking poll: check if there's data on the pipe
        local available = pipe_available(pipeHandle)
        if not available or available == 0 then
            return  -- nothing to read, skip this tick
        end

        -- Read the frame header
        local cmd, payloadLen = readFrameHeader(pipeHandle)
        if not cmd then
            return  -- incomplete frame or pipe error
        end

        -- Read payload
        local payload = nil
        if payloadLen > 0 then
            payload = readBytes(pipeHandle, payloadLen)
            if not payload then return end
        end

        -- Dispatch by command type
        if cmd == CMD_EXECUTE_SCRIPT then
            if not payload then return end

            stateFlags = STATE_EXECUTING

            -- Kill any previously running script (hot-swap)
            if activeCoroutine then
                pcall(coroutine.close, activeCoroutine)
                activeCoroutine = nil
            end

            -- Execute in a coroutine so we can hot-swap on next command
            activeCoroutine = coroutine.create(function()
                local ok, err = pcall(function()
                    local fn, compileErr = loadstring(payload)
                    if not fn then
                        sendResult(pipeHandle, RESULT_ERROR,
                            "Compile error: " .. tostring(compileErr))
                        return
                    end
                    fn()
                end)

                if not ok then
                    sendResult(pipeHandle, RESULT_ERROR,
                        "Runtime error: " .. tostring(err))
                    stateFlags = STATE_ERROR
                else
                    sendResult(pipeHandle, RESULT_OK, nil)
                    stateFlags = STATE_READY
                end
            end)

            -- Resume the coroutine (runs synchronously within this
            -- heartbeat tick; if it yields, it'll be resumed next tick)
            local coStatus, coErr = coroutine.resume(activeCoroutine)
            if not coStatus then
                sendResult(pipeHandle, RESULT_ERROR,
                    "Coroutine error: " .. tostring(coErr))
                stateFlags = STATE_ERROR
                activeCoroutine = nil
            end

            if coroutine.status(activeCoroutine) == "dead" then
                activeCoroutine = nil
                stateFlags = STATE_READY
            end

        elseif cmd == CMD_PING then
            sendPong(pipeHandle, stateFlags)

        elseif cmd == CMD_SHUTDOWN then
            if activeCoroutine then
                pcall(coroutine.close, activeCoroutine)
                activeCoroutine = nil
            end
            running = false
            scheduler.Heartbeat:Disconnect()
        end
    end)

    -- Initial ready signal — tell the controller we're alive
    sendPong(pipeHandle, STATE_READY)

    -- Keep the script alive (the heartbeat callback holds the reference).
    -- The target's scheduler drives this loop; we just need to prevent
    -- the function from returning while the Heartbeat connection is active.
    while running do
        -- If the target has a wait() or task.wait(), use it.
        -- Otherwise the heartbeat scheduler keeps this alive.
        pcall(function()
            task and task.wait(1)
        end)
    end
end
