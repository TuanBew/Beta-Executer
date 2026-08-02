-- scan_for_scriptcontext.lua — Route H: read-only full heap scan
-- Independent of any offset chain: walks the target's committed private
-- memory directly looking for an object whose ClassDescriptor->ClassName
-- resolves to "ScriptContext".

log("info", "=== Route H: ScriptContext heap scan ===")

local timeoutMs = 90000
log("info", "Scanning (timeout=" .. timeoutMs .. "ms)...")

local addr, err = scan_for_scriptcontext(timeoutMs)
if addr then
    log("info", string.format("FOUND ScriptContext @ 0x%X", addr))

    local hex, hexErr = dump_memory(addr, 512)
    if hex then
        log("info", "ScriptContext hex dump:")
        log("info", hex)
    else
        log("warn", "dump_memory failed: " .. tostring(hexErr))
    end

    write_file("docs/scriptcontext-scan-result.txt",
        string.format("ScriptContext = 0x%X\n\n%s", addr, hex or "(dump failed)"))
else
    log("error", "Scan failed: " .. tostring(err))
end

log("info", "=== Route H scan complete ===")
