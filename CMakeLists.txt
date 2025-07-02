# CMakeLists.txt
# This file is used to build the Sentinel application.
# It is used to configure the build environment and the dependencies.
# It is used to build the application.

# Set the minimum required CMake version
cmake_minimum_required(VERSION 3.16)

# --- Cross-Platform Vcpkg Integration ---
# Auto-detect vcpkg toolchain file across platforms
if(NOT CMAKE_TOOLCHAIN_FILE)
    # Try common vcpkg locations across platforms
    if(WIN32)
        # Windows common locations
        set(VCPKG_PATHS
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "C:/tools/vcpkg/scripts/buildsystems/vcpkg.cmake")
    elseif(APPLE)
        # macOS common locations
        set(VCPKG_PATHS
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            "/usr/local/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "/opt/homebrew/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    else()
        # Linux common locations
        set(VCPKG_PATHS
            "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            "/usr/local/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "/opt/vcpkg/scripts/buildsystems/vcpkg.cmake"
            "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    endif()
    
    # Find the first existing vcpkg toolchain file
    foreach(VCPKG_PATH ${VCPKG_PATHS})
        if(EXISTS "${VCPKG_PATH}")
            set(CMAKE_TOOLCHAIN_FILE "${VCPKG_PATH}" CACHE STRING "Vcpkg toolchain file")
            message(STATUS "Found vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
            break()
        endif()
    endforeach()
    
    # If not found, provide helpful error message
    if(NOT CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR 
            "Vcpkg toolchain file not found. Please:\n"
            "1. Install vcpkg: https://vcpkg.io/en/getting-started.html\n"
            "2. Set VCPKG_ROOT environment variable, or\n"
            "3. Set CMAKE_TOOLCHAIN_FILE to point to vcpkg.cmake\n"
            "Example: cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake")
    endif()
endif()

project(Sentinel LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform-specific compiler flags
if(MSVC)
    # Windows Visual Studio
    add_compile_options(/W4)
    # Enable parallel compilation
    add_compile_options(/MP)
else()
    # GCC/Clang (Linux/macOS)
    add_compile_options(-Wall -Wextra -Wpedantic)
    # Enable debug symbols in debug builds
    add_compile_options("$<$<CONFIG:Debug>:-g>")
    # Enable optimizations in release builds
    add_compile_options("$<$<CONFIG:Release>:-O3>")
endif()

# =============================================================================
# DEPENDENCY MANAGEMENT (FetchContent)
# This section ensures that all necessary third-party libraries are downloaded
# and available for the project to use.
# =============================================================================
include(FetchContent)

# --- Google Test ---
# We fetch GTest directly from GitHub. This ensures we use a consistent
# version and don't rely on a system-installed copy.
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For projects with C++11 or newer, this is all that is needed.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# --- Nlohmann JSON ---
# This library is header-only, but fetching it this way makes it available
# as a proper CMake target (nlohmann_json::nlohmann_json).
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Find top-level dependencies needed by multiple components
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Network Quick Qml QuickWidgets)

# Automatically run MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Add the library and application subdirectories
add_subdirectory(libs/core)
add_subdirectory(libs/gui)
add_subdirectory(apps/sentinel_gui)
add_subdirectory(apps/stream_cli)

# =============================================================================
# SUBDIRECTORIES
# Now that dependencies are declared, add our subdirectories.
# The 'tests' subdirectory will be able to find and use GTest.
# =============================================================================
# add_subdirectory(libs) # Or wherever your core library is
add_subdirectory(tests)
