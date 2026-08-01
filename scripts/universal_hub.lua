--[[
    universal_hub.lua — Modular Automation Script
    =============================================

    A modular LuaU payload for the Universal Hub framework.
    Each feature is a self-contained module with init/update/render hooks.

    Modules:
      - ConfigManager   — JSON config load/save/access
      - ObjectHighlight — Visual object highlighting
      - Movement        — Walk speed, jump power, hip height control
      - FOV             — Field of view adjustment
      - Environment     — Gravity, brightness, clock time control
      - CommLogger      — Communication object event logging

    FOR EDUCATIONAL DEMONSTRATION ONLY
]]

-- ============================================================
--  Global State
-- ============================================================

local UniversalHub = {
    modules = {},
    config = {},
    running = false,
    targetBase = 0,
    dataModel = 0,
    workspace = 0,
    localPlayer = 0,
}

-- ============================================================
--  Config Manager Module
-- ============================================================

local ConfigManager = {
    name = "ConfigManager",
    version = "1.0.0",
    configPath = "config/user_config.json",
    defaultConfigPath = "config/default_config.json",
}

function ConfigManager:init()
    print("[ConfigManager] Initializing...")

    -- Try loading user config first, fall back to default
    local ok, json = pcall(read_file, self.configPath)
    if ok and json then
        local success, tbl = pcall(json_decode, json)
        if success and tbl then
            UniversalHub.config = tbl
            print("[ConfigManager] Loaded user config: " .. self.configPath)
        end
    end

    -- If no user config, load defaults
    if next(UniversalHub.config) == nil then
        local ok2, defJson = pcall(read_file, self.defaultConfigPath)
        if ok2 and defJson then
            local success, tbl = pcall(json_decode, defJson)
            if success and tbl then
                UniversalHub.config = tbl
                print("[ConfigManager] Loaded default config")
            end
        end
    end

    -- If still empty, create minimal config
    if next(UniversalHub.config) == nil then
        UniversalHub.config = {
            version = "1.0.0",
            general = {
                auto_attach = true,
                target_process = "test.exe",
                hotkey_toggle = 45, -- VK_INSERT
            },
            visuals = {
                highlight_enabled = false,
                highlight_color = {0.0, 1.0, 0.0},
                highlight_radius = 30.0,
            },
            movement = {
                walk_speed = 16.0,
                jump_power = 50.0,
                hip_height = 0.0,
            },
            camera = {
                fov = 70.0,
            },
            environment = {
                gravity = 196.2,
                brightness = 2.0,
                clock_time = 14.0,
            },
            communication_logger = {
                enabled = false,
                log_incoming = true,
                log_outgoing = true,
            },
        }
        print("[ConfigManager] Created default config")
    end

    print("[ConfigManager] Ready")
end

function ConfigManager:save()
    local ok, json = pcall(json_encode, UniversalHub.config)
    if ok and json then
        pcall(write_file, self.configPath, json)
        gui_log("[ConfigManager] Config saved to " .. self.configPath)
    else
        gui_log("[ConfigManager] ERROR: Failed to encode config")
    end
end

-- Get a config value by dotted path (e.g., "movement.walk_speed")
function ConfigManager:get(path, default)
    local keys = {}
    for key in string.gmatch(path, "[^.]+") do
        table.insert(keys, key)
    end

    local current = UniversalHub.config
    for _, key in ipairs(keys) do
        if type(current) ~= "table" then
            return default
        end
        current = current[key]
        if current == nil then
            return default
        end
    end

    return current
end

-- Set a config value by dotted path
function ConfigManager:set(path, value)
    local keys = {}
    for key in string.gmatch(path, "[^.]+") do
        table.insert(keys, key)
    end

    local current = UniversalHub.config
    for i = 1, #keys - 1 do
        if current[keys[i]] == nil then
            current[keys[i]] = {}
        end
        current = current[keys[i]]
    end

    current[keys[#keys]] = value
end

-- ============================================================
--  Object Highlight Module
-- ============================================================

local ObjectHighlight = {
    name = "ObjectHighlight",
    version = "1.0.0",
    enabled = false,
    color = {0.0, 1.0, 0.0},
    radius = 30.0,
}

function ObjectHighlight:init()
    print("[ObjectHighlight] Initializing...")

    self.enabled = ConfigManager:get("visuals.highlight_enabled", false)
    local cfgColor = ConfigManager:get("visuals.highlight_color", {0.0, 1.0, 0.0})
    self.color = cfgColor
    self.radius = ConfigManager:get("visuals.highlight_radius", 30.0)

    print("[ObjectHighlight] Ready (enabled=" .. tostring(self.enabled) .. ")")
end

function ObjectHighlight:render()
    -- Register GUI widgets for this module
    local tab = "Visuals"

    gui_add_checkbox(tab, "Enable Highlight", self.enabled, function(val)
        self.enabled = val
        ConfigManager:set("visuals.highlight_enabled", val)
        if val then
            gui_log("[Highlight] Enabled")
        else
            gui_log("[Highlight] Disabled")
        end
    end)

    gui_add_slider(tab, "Radius", 1.0, 100.0, self.radius, function(val)
        self.radius = val
        ConfigManager:set("visuals.highlight_radius", val)
    end)

    gui_add_color_picker(tab, "Color",
        self.color[1], self.color[2], self.color[3], 1.0,
        function(r, g, b, a)
            self.color = {r, g, b}
            ConfigManager:set("visuals.highlight_color", {r, g, b})
        end
    )
end

function ObjectHighlight:update()
    if not self.enabled then return end
    -- In a full implementation, this would write highlight values
    -- to the target process's rendering structures at known offsets
end

-- ============================================================
--  Movement Module
-- ============================================================

local Movement = {
    name = "Movement",
    version = "1.0.0",
    walkSpeed = 16.0,
    jumpPower = 50.0,
    hipHeight = 0.0,
    playerAddr = 0,
}

function Movement:init()
    print("[Movement] Initializing...")

    self.walkSpeed = ConfigManager:get("movement.walk_speed", 16.0)
    self.jumpPower = ConfigManager:get("movement.jump_power", 50.0)
    self.hipHeight = ConfigManager:get("movement.hip_height", 0.0)

    -- Set up default values in the target if possible
    self:applyDefaults()

    print("[Movement] Ready (ws=" .. self.walkSpeed .. ")")
end

function Movement:applyDefaults()
    -- Apply the configured movement values to the target process
    if UniversalHub.localPlayer == 0 then return end

    -- WalkSpeed at offset 0x1d0
    local wsAddr = UniversalHub.localPlayer + 0x1d0
    local success, err = pcall(write_memory, wsAddr, self.walkSpeed, "f32")

    -- JumpPower at offset 0x1a4
    local jpAddr = UniversalHub.localPlayer + 0x1a4
    pcall(write_memory, jpAddr, self.jumpPower, "f32")

    -- HipHeight at offset 0x194
    local hhAddr = UniversalHub.localPlayer + 0x194
    pcall(write_memory, hhAddr, self.hipHeight, "f32")
end

function Movement:render()
    local tab = "Automation"

    gui_add_slider(tab, "Walk Speed", 0.0, 200.0, self.walkSpeed, function(val)
        self.walkSpeed = val
        ConfigManager:set("movement.walk_speed", val)

        if UniversalHub.localPlayer ~= 0 then
            local addr = UniversalHub.localPlayer + 0x1d0
            pcall(write_memory, addr, val, "f32")
        end
        gui_log(string.format("[Movement] WalkSpeed = %.1f", val))
    end)

    gui_add_slider(tab, "Jump Power", 0.0, 500.0, self.jumpPower, function(val)
        self.jumpPower = val
        ConfigManager:set("movement.jump_power", val)

        if UniversalHub.localPlayer ~= 0 then
            local addr = UniversalHub.localPlayer + 0x1a4
            pcall(write_memory, addr, val, "f32")
        end
        gui_log(string.format("[Movement] JumpPower = %.1f", val))
    end)

    gui_add_slider(tab, "Hip Height", -5.0, 20.0, self.hipHeight, function(val)
        self.hipHeight = val
        ConfigManager:set("movement.hip_height", val)

        if UniversalHub.localPlayer ~= 0 then
            local addr = UniversalHub.localPlayer + 0x194
            pcall(write_memory, addr, val, "f32")
        end
        gui_log(string.format("[Movement] HipHeight = %.1f", val))
    end)
end

function Movement:update()
    -- No continuous updates needed; writes happen on slider change
end

-- ============================================================
--  FOV Module
-- ============================================================

local FOVModule = {
    name = "FOV",
    version = "1.0.0",
    fov = 70.0,
}

function FOVModule:init()
    print("[FOV] Initializing...")
    self.fov = ConfigManager:get("camera.fov", 70.0)
    print("[FOV] Ready (fov=" .. self.fov .. ")")
end

function FOVModule:render()
    local tab = "Automation"

    gui_add_slider(tab, "Field of View", 1.0, 120.0, self.fov, function(val)
        self.fov = val
        ConfigManager:set("camera.fov", val)
        gui_log(string.format("[FOV] FOV = %.0f", val))
    end)
end

function FOVModule:update()
    -- In a full implementation, write FOV to camera offset
end

-- ============================================================
--  Environment Module
-- ============================================================

local Environment = {
    name = "Environment",
    version = "1.0.0",
    gravity = 196.2,
    brightness = 2.0,
    clockTime = 14.0,
}

function Environment:init()
    print("[Environment] Initializing...")

    self.gravity = ConfigManager:get("environment.gravity", 196.2)
    self.brightness = ConfigManager:get("environment.brightness", 2.0)
    self.clockTime = ConfigManager:get("environment.clock_time", 14.0)

    print("[Environment] Ready (gravity=" .. self.gravity .. ")")
end

function Environment:render()
    local tab = "Automation"

    gui_add_slider(tab, "Gravity", 0.0, 500.0, self.gravity, function(val)
        self.gravity = val
        ConfigManager:set("environment.gravity", val)
        gui_log(string.format("[Environment] Gravity = %.1f", val))
    end)

    gui_add_slider(tab, "Brightness", 0.0, 10.0, self.brightness, function(val)
        self.brightness = val
        ConfigManager:set("environment.brightness", val)
        gui_log(string.format("[Environment] Brightness = %.2f", val))
    end)

    gui_add_slider(tab, "Clock Time", 0.0, 24.0, self.clockTime, function(val)
        self.clockTime = val
        ConfigManager:set("environment.clock_time", val)
        gui_log(string.format("[Environment] ClockTime = %.1f", val))
    end)
end

function Environment:update()
    -- Apply environment values continuously if needed
end

-- ============================================================
--  Communication Logger Module
-- ============================================================

local CommLogger = {
    name = "CommLogger",
    version = "1.0.0",
    enabled = false,
    logIncoming = true,
    logOutgoing = true,
    trackedRemotes = {},
    pollInterval = 2.0,  -- seconds between scans
    lastPollTime = 0,
    frameCount = 0,
}

function CommLogger:init()
    print("[CommLogger] Initializing...")

    self.enabled = ConfigManager:get("communication_logger.enabled", false)
    self.logIncoming = ConfigManager:get("communication_logger.log_incoming", true)
    self.logOutgoing = ConfigManager:get("communication_logger.log_outgoing", true)

    -- Scan for communication objects
    if self.enabled then
        self:scanRemotes()
    end

    print("[CommLogger] Ready (enabled=" .. tostring(self.enabled) .. ")")
end

function CommLogger:scanRemotes()
    local ok, remotes = pcall(get_remote_events)
    if not ok then
        gui_log("[CommLogger] ERROR: Failed to scan for communication objects: " ..
                tostring(remotes))
        return
    end

    if type(remotes) ~= "table" then return end

    self.trackedRemotes = {}
    for _, remote in ipairs(remotes) do
        table.insert(self.trackedRemotes, {
            name = remote.name,
            type = remote.type,
            address = remote.address,
        })
        gui_log(string.format("[CommLogger] Found: %s (%s) @ 0x%X",
               remote.name, remote.type, remote.address))
    end

    if #self.trackedRemotes > 0 then
        gui_log(string.format("[CommLogger] Tracking %d communication objects",
               #self.trackedRemotes))
    else
        gui_log("[CommLogger] No communication objects found")
    end
end

function CommLogger:render()
    local tab = "Objects"

    gui_add_checkbox(tab, "Enable Communication Logger", self.enabled, function(val)
        self.enabled = val
        ConfigManager:set("communication_logger.enabled", val)
        if val then
            gui_log("[CommLogger] Enabled — scanning for communication objects...")
            self:scanRemotes()
        else
            gui_log("[CommLogger] Disabled")
        end
    end)

    gui_add_checkbox(tab, "Log Incoming", self.logIncoming, function(val)
        self.logIncoming = val
        ConfigManager:set("communication_logger.log_incoming", val)
    end)

    gui_add_checkbox(tab, "Log Outgoing", self.logOutgoing, function(val)
        self.logOutgoing = val
        ConfigManager:set("communication_logger.log_outgoing", val)
    end)

    -- Show tracked remotes
    if #self.trackedRemotes > 0 then
        gui_add_text(tab, "Tracked Objects:")
        for _, remote in ipairs(self.trackedRemotes) do
            gui_add_text(tab,
                string.format("  [%s] %s @ 0x%X",
                remote.type, remote.name, remote.address))
        end
    end

    -- Manual rescan button
    gui_add_button(tab, "Rescan Communication Objects", function()
        gui_log("[CommLogger] Manual rescan triggered")
        self:scanRemotes()
    end)
end

function CommLogger:update()
    if not self.enabled then return end

    -- Periodic polling of communication object state
    self.frameCount = self.frameCount + 1

    -- Approximate polling (60 FPS ≈ 120 frames for 2 seconds)
    if self.frameCount % 120 == 0 then
        for _, remote in ipairs(self.trackedRemotes) do
            -- Read the communication object's state/value at known offsets
            -- This would detect if a signal was fired since last check
            local ok, val = pcall(read_memory, remote.address + 0xb8, "i32")
            if ok and val and val ~= 0 then
                gui_log(string.format("[CommLogger] Activity detected: %s (%s) — value=%d",
                       remote.name, remote.type, val))
            end
        end
    end
end

-- ============================================================
--  Module Registration & Lifecycle
-- ============================================================

function UniversalHub:registerModule(module)
    table.insert(self.modules, module)
    print("[Hub] Registered module: " .. module.name .. " v" .. module.version)
end

function UniversalHub:resolveTarget()
    -- Try to find key addresses in the target
    local moduleName = ConfigManager:get("general.target_process", "test.exe")
    local ok, base = pcall(get_module_base, moduleName)
    if ok and base then
        self.targetBase = base
        print(string.format("[Hub] Target base: 0x%X", base))

        -- Try to resolve DataModel chain
        -- FakeDataModel → DataModel → Workspace → LocalPlayer
        local fakeDMPtr = 0x7e26978  -- FakeDataModelPointer
        local ok2, fakeDM = pcall(read_memory, base + fakeDMPtr, "u32")
        if ok2 and fakeDM ~= 0 then
            local ok3, dm = pcall(read_memory, fakeDM + 0x1d0, "u32")
            if ok3 and dm ~= 0 then
                self.dataModel = dm
                local ok4, ws = pcall(read_memory, dm + 0x160, "u32")
                if ok4 and ws ~= 0 then
                    self.workspace = ws
                    print(string.format("[Hub] Workspace: 0x%X", ws))

                    -- Find local player
                    local ok5, lp = pcall(read_memory, dm + 0x130, "u32")
                    if ok5 and lp ~= 0 then
                        self.localPlayer = lp
                        print(string.format("[Hub] LocalPlayer: 0x%X", lp))
                    end
                end
            end
        end
    end
end

function UniversalHub:init()
    print("")
    print("=== Universal Hub v1.0.0 ===")
    print("Modular Automation Framework")
    print("FOR EDUCATIONAL DEMONSTRATION ONLY")
    print("")

    -- 1. Initialize ConfigManager first (other modules depend on it)
    ConfigManager:init()
    self:registerModule(ConfigManager)

    -- 2. Resolve target addresses
    self:resolveTarget()

    -- 3. Register feature modules
    ObjectHighlight:init()
    self:registerModule(ObjectHighlight)

    Movement:init()
    self:registerModule(Movement)

    FOVModule:init()
    self:registerModule(FOVModule)

    Environment:init()
    self:registerModule(Environment)

    CommLogger:init()
    self:registerModule(CommLogger)

    self.running = true

    print("")
    print("[Hub] All modules initialized. Running.")
    gui_log("=== Universal Hub v1.0.0 Ready ===")
    gui_log(string.format("[Hub] %d modules loaded", #self.modules))
end

function UniversalHub:update()
    if not self.running then return end

    for _, module in ipairs(self.modules) do
        if module.update then
            local ok, err = pcall(module.update, module)
            if not ok then
                gui_log("[Hub] ERROR in " .. module.name .. ".update(): " .. tostring(err))
            end
        end
    end
end

function UniversalHub:render()
    if not self.running then return end

    for _, module in ipairs(self.modules) do
        if module.render then
            local ok, err = pcall(module.render, module)
            if not ok then
                gui_log("[Hub] ERROR in " .. module.name .. ".render(): " .. tostring(err))
            end
        end
    end
end

function UniversalHub:shutdown()
    print("[Hub] Shutting down...")
    self.running = false

    -- Save config before exit
    ConfigManager:save()

    print("[Hub] Goodbye.")
end

-- ============================================================
--  Global Hook — called from C++ each frame
-- ============================================================

-- _G["on_frame"] is called by LuaBridge::OnFrame() every render frame
function on_frame()
    UniversalHub:update()
    UniversalHub:render()
end

-- ============================================================
--  Auto-initialize
-- ============================================================

-- Initialize immediately when the script is loaded
-- (called from C++ via ExecuteFile or ExecuteString)
UniversalHub:init()
