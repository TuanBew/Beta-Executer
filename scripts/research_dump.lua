-- research_dump.lua — Hex dump collector for privilege resolution research
-- Dumps all critical objects to docs/memory-dump.txt for offline analysis.

log("info", "=== Research Dump Script v1.0 ===")

local output = {}
local function emit(line)
    output[#output + 1] = line
    log("info", line)
end

emit("# Privilege Resolution Memory Dumps")
emit("# Generated: " .. os.date("%Y-%m-%d %H:%M:%S"))
emit("")

-- Phase 1: Enumerate all TaskScheduler jobs
emit("## TaskScheduler Jobs")
emit("")
local jobs = enumerate_jobs()
if jobs then
    emit("Total jobs: " .. #jobs)
    for i, job in ipairs(jobs) do
        local addr = string.format("0x%X", job.address or 0)
        emit(string.format("  #%d  addr=%s  name='%s'  class='%s'",
            i, addr, job.name or "", job.className or ""))
    end
else
    emit("enumerate_jobs() returned nil")
end
emit("")

-- Phase 2: Resolve context and dump all addresses
emit("## Context Resolution")
emit("")
local ctx = resolve_context()
if ctx then
    emit("Attached:        " .. tostring(ctx.attached))
    emit("Resolved:        " .. tostring(ctx.resolved))
    emit("Module Base:     " .. string.format("0x%X", ctx.moduleBase or 0))
    emit("FakeDataModel:   " .. string.format("0x%X", ctx.fakeDataModel or 0))
    emit("DataModel:       " .. string.format("0x%X", ctx.dataModel or 0))
    emit("ScriptContext:   " .. string.format("0x%X", ctx.scriptContext or 0))
    emit("Current Level:   " .. tostring(ctx.currentLevel))
    emit("Resolution Path: " .. tostring(ctx.resolutionPath))
    emit("Candidates:      " .. tostring(ctx.candidateCount))
    emit("Paths tried:     A=" .. tostring(ctx.pathATried) ..
         " B=" .. tostring(ctx.pathBTried) ..
         " C=" .. tostring(ctx.pathCTried))
    if ctx.error then
        emit("Error: " .. ctx.error)
    end
else
    emit("resolve_context() returned nil")
end
emit("")

-- Phase 3: Hex dump critical objects
local function dumpObject(label, addr, size)
    if not addr or addr == 0 then
        emit("## " .. label .. " — address is null, skipping")
        emit("")
        return
    end
    emit("## " .. label .. " @ " .. string.format("0x%X", addr))
    emit("")
    local hex, err = dump_memory(addr, size or 512)
    if hex then
        emit(hex)
    else
        emit("FAILED: " .. tostring(err))
    end
    emit("")
end

-- Dump FakeDataModel
if ctx and ctx.fakeDataModel and ctx.fakeDataModel ~= 0 then
    dumpObject("FakeDataModel", ctx.fakeDataModel, 512)

    -- Also dump what FakeDataModel+0x1D0 points to
    local dmA, err = read_memory(ctx.fakeDataModel + 0x1D0, "i64")
    if dmA and dmA ~= 0 then
        dumpObject("FakeDataModel+0x1D0 (PathA DataModel candidate)", dmA, 512)
    end
end

-- Dump DataModel if resolved (or best-effort candidate on failure).
-- Wide dump (2048 bytes) so we can scan past the old 0x440/0x70/0x78
-- offsets in case they've drifted for this build.
if ctx and ctx.dataModel and ctx.dataModel ~= 0 then
    dumpObject("Resolved/Candidate DataModel", ctx.dataModel, 2048)
end

-- Dump ScriptContext if resolved
if ctx and ctx.scriptContext and ctx.scriptContext ~= 0 then
    dumpObject("Resolved ScriptContext", ctx.scriptContext, 512)
end

-- Phase 4: Find and dump ScriptContextTaskQueue job
if jobs then
    for i, job in ipairs(jobs) do
        local lower = string.lower(job.name or "")
        if string.find(lower, "scriptcontext") then
            dumpObject("ScriptContextTaskQueue job '" .. job.name .. "'",
                       job.address, 512)
        end
        if string.find(lower, "render") then
            dumpObject("RenderJob '" .. job.name .. "'", job.address, 512)

            -- Also dump what RenderJob+0x1C8 points to
            local dmB, err = read_memory(job.address + 0x1C8, "i64")
            if dmB and dmB ~= 0 then
                dumpObject("RenderJob+0x1C8 (PathB DataModel candidate)", dmB, 512)
            end
        end
    end
end

-- Phase 5: Write to file
local content = table.concat(output, "\n")
local ok, err = write_file("docs/memory-dump.txt", content)
if ok then
    log("info", "Research dump saved to docs/memory-dump.txt")
else
    log("error", "Failed to save dump: " .. tostring(err))
end

log("info", "=== Research Dump Complete ===")
