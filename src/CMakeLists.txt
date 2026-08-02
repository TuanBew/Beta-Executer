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
)

# Bundle Capcom.sys into build output
add_custom_command(TARGET UniversalHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_SOURCE_DIR}/scripts/Capcom.sys
    $<TARGET_FILE_DIR:UniversalHub>/Capcom.sys
)

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

add_custom_command(
    OUTPUT ${SCRIPT_MANAGER_INC}
    COMMAND powershell -NoProfile -Command
        "[System.IO.File]::WriteAllText('${SCRIPT_MANAGER_INC}',
         'R"lua(' + [System.IO.File]::ReadAllText('${SCRIPT_MANAGER_SRC}') + ')lua"')"
    DEPENDS ${SCRIPT_MANAGER_SRC}
    COMMENT "Embedding ScriptManager.lua as C array"
)

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

# No CRT dependency — reduces DLL size, avoids CRT init in target
set_target_properties(PayloadDLL PROPERTIES
    WIN32_EXECUTABLE FALSE
)
