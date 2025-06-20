# CMakeLists.txt
# This file is used to build the Sentinel application.
# It is used to configure the build environment and the dependencies.
# It is used to build the application.

# Set the minimum required CMake version
cmake_minimum_required(VERSION 3.16)
project(Sentinel LANGUAGES CXX)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set the path for `find_package` to look for custom modules
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

# Find top-level dependencies needed by multiple components
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Charts Network)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(nlohmann_json REQUIRED)


# Automatically run MOC, UIC, and RCC
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)


# Add the library and application subdirectories
add_subdirectory(libs/core)
add_subdirectory(libs/gui)
add_subdirectory(apps/sentinel_gui)
add_subdirectory(apps/stream_cli)
