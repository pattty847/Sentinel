# CMakeLists.txt
# This file is used to build the Sentinel application.
# It is used to configure the build environment and the dependencies.
# It is used to build the application.

# Set the minimum required CMake version
cmake_minimum_required(VERSION 3.16)

# --- Vcpkg Integration ---
# If CMAKE_TOOLCHAIN_FILE is not set, set it to the default vcpkg location.
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE /Users/patrickmcdermott/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "Vcpkg toolchain file")
endif()

project(Sentinel LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# =============================================================================
# SUBDIRECTORIES
# Now that dependencies are declared, add our subdirectories.
# The 'tests' subdirectory will be able to find and use GTest.
# =============================================================================
# add_subdirectory(libs) # Or wherever your core library is
add_subdirectory(tests)
