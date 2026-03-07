cmake_minimum_required(VERSION 3.22)

# Reduce CMake message verbosity (only show STATUS and above, suppress NOTICE)
if(NOT DEFINED CMAKE_MESSAGE_LOG_LEVEL)
    set(CMAKE_MESSAGE_LOG_LEVEL STATUS CACHE STRING "CMake log level" FORCE)
endif()

# ────────────────────────────────────────────────────────────────
# PROJECT
# ────────────────────────────────────────────────────────────────
project(sentinel LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(WIN32)
    set(SENTINEL_PLATFORM "Windows")
    add_definitions(-D_WIN32_WINNT=0x0A00)
elseif(APPLE)
    set(SENTINEL_PLATFORM "macOS")
elseif(UNIX)
    set(SENTINEL_PLATFORM "Linux")
endif()

option(SENTINEL_USE_GPU "Enable experimental GPU compute paths" OFF)

# ────────────────────────────────────────────────────────────────
# MSVC FIXES (Do not remove)
# ────────────────────────────────────────────────────────────────
if (MSVC)
    # (/EHsc) ensures proper C++ exception handling semantics in MSVC, silencing noisy warnings about unhandled exception models.
    # (/bigobj) enables larger object files, improving build performance and reducing the number of files in the build directory.
    # Changes obj file size from 65,279 addressable sections to 4,294,967,295 addressable sections, useful for boost::beast.
    add_compile_options(/EHsc /bigobj)

    # Release flags
    # (/O2 /GL /DNDEBUG) sets MSVC release optimizations: /O2 enables maximum optimization, /GL enables whole-program optimization,
    # and /DNDEBUG disables debug code to produce efficient release binaries.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL /DNDEBUG")
endif()

# ────────────────────────────────────────────────────────────────
# DEPENDENCIES
# All C++ libraries are resolved via vcpkg toolchain on all platforms.
# ────────────────────────────────────────────────────────────────
include(FetchContent)

# JSON
find_package(nlohmann_json CONFIG REQUIRED)

# OpenSSL
find_package(OpenSSL REQUIRED)

# BOOST — IMPORTANT: beast + system + headers
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost REQUIRED
    COMPONENTS system headers
)

# JWT-CPP (header-only, resolved via vcpkg)
# Try find_package first (if vcpkg provides a config), fallback to find_path
find_package(jwt-cpp CONFIG QUIET)
if(NOT jwt-cpp_FOUND)
    find_path(JWT_CPP_INCLUDE_DIR "jwt-cpp/jwt.h" REQUIRED)
    add_library(jwt-cpp::jwt-cpp INTERFACE IMPORTED)
    target_include_directories(jwt-cpp::jwt-cpp INTERFACE "${JWT_CPP_INCLUDE_DIR}")
endif()

# MSDFGen (GPU text atlas generation)
find_package(msdfgen CONFIG REQUIRED)

# Googletest (tests only)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
if (WIN32)
    # Disable pthreads on Windows to avoid noisy detection spam
    set(gtest_disable_pthreads ON CACHE BOOL "" FORCE)
endif()
FetchContent_MakeAvailable(googletest)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Network WebSockets Quick Qml QuickWidgets QuickControls2 ShaderTools Svg Test)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ────────────────────────────────────────────────────────────────
# QML POLICY CLEANUP (kills Qt spam)
# Guard for older Qt (e.g., 6.4) where qt_policy is unavailable.
# ────────────────────────────────────────────────────────────────
if (COMMAND qt_policy)
    qt_policy(SET QTP0001 NEW)
    qt_policy(SET QTP0004 NEW)
endif()

# ────────────────────────────────────────────────────────────────
# SUBDIRECTORIES
# ────────────────────────────────────────────────────────────────
add_subdirectory(libs/core)
add_subdirectory(libs/gui)
add_subdirectory(apps/sentinel_gui apps/sentinel-gui)
add_subdirectory(apps/sentinel-server)
enable_testing()
add_subdirectory(tests/marketdata)
add_subdirectory(tests/render)
add_subdirectory(tests/trading)

# ────────────────────────────────────────────────────────────────
# LOG
# ────────────────────────────────────────────────────────────────
message(STATUS "=== Sentinel Build Configuration ===")
message(STATUS "Platform: ${SENTINEL_PLATFORM}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "GPU Experimental: ${SENTINEL_USE_GPU}")
message(STATUS "=====================================")
