# CMakeLists.txt
# This file is used to build the Sentinel application.
# It is used to configure the build environment and the dependencies.
# It is used to build the application.

# Set the minimum required CMake version
cmake_minimum_required(VERSION 3.16)

# --- Cross-Platform Configuration ---
# Detect operating system
if(WIN32)
    set(SENTINEL_PLATFORM "Windows")
    set(SENTINEL_PLATFORM_WINDOWS TRUE)
    set(CMAKE_PREFIX_PATH "C:/msys64/mingw64" ${CMAKE_PREFIX_PATH})
elseif(APPLE)
    set(SENTINEL_PLATFORM "macOS")
    set(SENTINEL_PLATFORM_MACOS TRUE)
elseif(UNIX)
    set(SENTINEL_PLATFORM "Linux")
    set(SENTINEL_PLATFORM_LINUX TRUE)
endif()

project(Sentinel LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- GPU Acceleration Support ---
option(SENTINEL_USE_GPU "Enable GPU acceleration" OFF)
option(SENTINEL_USE_CUDA "Enable CUDA support" OFF)

if(SENTINEL_USE_GPU)
    message(STATUS "GPU acceleration enabled")
    
    # Find CUDA if requested
    if(SENTINEL_USE_CUDA)
        find_package(CUDA QUIET)
        if(CUDA_FOUND)
            message(STATUS "CUDA found: ${CUDA_VERSION}")
            set(SENTINEL_CUDA_AVAILABLE TRUE)
        else()
            message(WARNING "CUDA requested but not found")
        endif()
    endif()
    
    # Find OpenGL for GPU rendering
    find_package(OpenGL REQUIRED)
    if(OpenGL_FOUND)
        message(STATUS "OpenGL found: ${OpenGL_VERSION}")
        set(SENTINEL_OPENGL_AVAILABLE TRUE)
    endif()
endif()

# --- Performance Optimization ---
include(CheckCXXCompilerFlag)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        # MSVC specific flags
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /GL /DNDEBUG")
    else()
        # Flags for other compilers (GCC, Clang, etc.)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
        
        # You can still have OS-specific flags here
        if(APPLE)
             set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")
        endif()
    endif()
endif()

# =============================================================================
# DEPENDENCY MANAGEMENT
# =============================================================================
include(FetchContent)

# --- System packages (Windows uses MSYS2, others use system package managers) ---
if(WIN32)
    # Windows: Use MSYS2 packages
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPENSSL REQUIRED openssl)
    find_package(nlohmann_json REQUIRED)
else()
    # macOS/Linux: Use FetchContent as before
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# --- JWT-CPP (header-only, always use FetchContent) ---
FetchContent_Declare(
  jwt-cpp
  GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG v0.7.0
)
FetchContent_MakeAvailable(jwt-cpp)

# --- Google Test ---
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Find top-level dependencies needed by multiple components
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Network Quick Qml QuickWidgets Test)

# Automatically run MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Add the library and application subdirectories
add_subdirectory(libs/core)
add_subdirectory(libs/gui)
add_subdirectory(apps/sentinel_gui)
add_subdirectory(apps/stream_cli)
# add_subdirectory(tests)  # TODO: Update tests for V2 architecture
enable_testing()
add_subdirectory(tests/marketdata)

# --- Print Configuration Summary ---
message(STATUS "=== Sentinel Build Configuration ===")
message(STATUS "Platform: ${SENTINEL_PLATFORM}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "GPU Acceleration: ${SENTINEL_USE_GPU}")
if(SENTINEL_USE_GPU)
    message(STATUS "CUDA Available: ${SENTINEL_CUDA_AVAILABLE}")
    message(STATUS "OpenGL Available: ${SENTINEL_OPENGL_AVAILABLE}")
endif()
message(STATUS "=====================================")