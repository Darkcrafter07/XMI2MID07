cmake_minimum_required(VERSION 3.10)

# Project name and enabled languages
project(XMI2MID LANGUAGES C CXX)

# Set C++11 and C11 standards
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Option to toggle Windows XP 32-bit compatibility settings
option(BUILD_FOR_WINXP "Configure build for Windows XP 32-bit" ON)

# Define executable target with your source file
add_executable(${PROJECT_NAME} XMI2MID.C)

# Microsoft Visual C++ (MSVC) specific settings
if(MSVC)
    # Use static runtime linking (/MT instead of /MD) so the executable runs on clean XP without vcredist
    set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    
    # Disable MSVC security deprecation warnings
    target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)

    if(BUILD_FOR_WINXP)
        # Target Windows XP (0x0501)
        target_compile_definitions(${PROJECT_NAME} PRIVATE 
            _WIN32_WINNT=0x0501 
            WINVER=0x0501
        )
        # Set subsystem version to 5.01 (Windows XP) for the linker
        set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "/SUBSYSTEM:CONSOLE,5.01")
    endif()
endif()

# Cross-platform compatibility for GCC / Clang (Linux / macOS)
if(NOT MSVC)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "_byteswap_ulong=__builtin_bswap32")
endif()
