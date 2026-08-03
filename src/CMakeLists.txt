# Universal Hub — Main Executable

find_package(OpenGL REQUIRED)

# ---- ImGui GLFW/OpenGL3 backend library ----

add_library(imgui_backend STATIC
    ${IMGUI_SOURCES}
)
target_include_directories(imgui_backend PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_backend PUBLIC
    OpenGL::GL
    glfw
)

# ---- Injector — Kernel-assisted injection library (Phase 2) ----

add_library(Injector STATIC
    Injector/CapcomDriver.cpp
    Injector/KernelExec.cpp
    Injector/ManualMapInjector.cpp
    Injector/UserModeMapper.cpp
)

target_include_directories(Injector PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ---- Universal Hub Executable ----

add_executable(UniversalHub
    main.cpp
    Core/Engine.cpp
    Core/Bootstrap.cpp
    Core/PrivilegeElevation.cpp
    Logging/Logger.cpp
    Logging/CrashHandler.cpp
    Lua/LuaBridge.cpp
    GUI/GUI.cpp
)

target_include_directories(UniversalHub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(UniversalHub PRIVATE
    imgui_backend
    Injector
    glfw
    OpenGL::GL
    nlohmann_json::nlohmann_json
    Luau.VM
    Luau.Compiler
    Luau.Config
    dbghelp
    comdlg32
    ntdll
)

target_sources(UniversalHub PRIVATE
    Protocol/PipeServer.cpp
)

target_include_directories(UniversalHub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Payload   # for PipeProtocol.h
)

# Copy scripts and config to build directory
add_custom_command(TARGET UniversalHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/scripts
    $<TARGET_FILE_DIR:UniversalHub>/scripts
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:UniversalHub>/config
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:PayloadDLL>
    $<TARGET_FILE_DIR:UniversalHub>/PayloadDLL.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:NullDLL>
    $<TARGET_FILE_DIR:UniversalHub>/NullDLL.dll
)

# Capcom.sys (BYOVD kernel driver) is NOT bundled — the project's kernel R/W
# path is optional. CapcomDriver.cpp falls back to user-mode RPM/WPM when the
# driver is absent, and Bootstrap falls back to CreateRemoteThread+LoadLibraryA.
# For educational demonstration in controlled offline environments, the user-mode
# path is fully sufficient. To enable the kernel path, place Capcom.sys alongside
# UniversalHub.exe and run as Administrator.

# Windows-specific: require admin for process access
if(WIN32)
    set_target_properties(UniversalHub PROPERTIES
        WIN32_EXECUTABLE FALSE  # Console for now (useful for logging during Phase 1-2)
    )
endif()

# ---- PayloadDLL — Injected into target process (Phase 1) ----

# Generate C array from ScriptManager.lua
set(SCRIPT_MANAGER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/Payload/ScriptManager.lua")
set(SCRIPT_MANAGER_INC "${CMAKE_CURRENT_BINARY_DIR}/ScriptManager.lua.inc")

# Embed ScriptManager.lua as a C++ raw string literal at configure time.
# CMake file(READ)/file(WRITE) avoids PowerShell escaping issues.
file(READ "${SCRIPT_MANAGER_SRC}" SCRIPT_MANAGER_CONTENT)
file(WRITE "${SCRIPT_MANAGER_INC}" "R\"lua(${SCRIPT_MANAGER_CONTENT})lua\"\n")

add_library(PayloadDLL SHARED
    Payload/PayloadDLL.cpp
    ${SCRIPT_MANAGER_INC}
)

target_include_directories(PayloadDLL PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}     # for ScriptManager.lua.inc
)

target_link_libraries(PayloadDLL PRIVATE
    Luau.VM
    Luau.Compiler
    Luau.Config
)

# Static CRT is set globally in the root CMakeLists.txt — the DLL is
# injected into a target process that does not have VC++ runtime DLLs.
set_target_properties(PayloadDLL PROPERTIES
    WIN32_EXECUTABLE FALSE
)

# NullDLL — minimal injection test payload (does nothing, confirms LoadLibraryA path)
add_library(NullDLL SHARED
    Payload/null_dll.cpp
)
set_target_properties(NullDLL PROPERTIES
    WIN32_EXECUTABLE FALSE
)
