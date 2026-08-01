# Mock Test Target — CMake

add_executable(mock_test_exe
    main.cpp
)

target_compile_features(mock_test_exe PRIVATE cxx_std_17)

# Build as a console app for easy PID output
if(WIN32)
    set_target_properties(mock_test_exe PROPERTIES
        WIN32_EXECUTABLE FALSE
    )
endif()
