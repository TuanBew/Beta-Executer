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
    glfw
    OpenGL::GL
    nlohmann_json::nlohmann_json
    Luau.VM
    Luau.Compiler
    Luau.Config
    dbghelp
)

# Copy scripts and config to build directory
add_custom_command(TARGET UniversalHub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/scripts
    $<TARGET_FILE_DIR:UniversalHub>/scripts
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/config
    $<TARGET_FILE_DIR:UniversalHub>/config
)

# Windows-specific: require admin for process access
if(WIN32)
    set_target_properties(UniversalHub PROPERTIES
        WIN32_EXECUTABLE FALSE  # Console for now (useful for logging during Phase 1-2)
    )
endif()
