# =============================================================================
# SENTINEL TESTS - CMakeLists.txt
# Defines test executables and links them against GTest and the core library.
# =============================================================================

cmake_minimum_required(VERSION 3.22)

include(FetchContent)
# Fetch GoogleTest at configure time
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# This allows CTest to discover and run the tests defined here.
enable_testing()

# Set the C++ standard for all test targets in this file.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# Test Target: test_messageparser
# A standalone unit test for the JSON parser. It has no Qt dependencies.
# -----------------------------------------------------------------------------
add_executable(test_messageparser test_messageparser.cpp)

target_link_libraries(test_messageparser PRIVATE
  sentinel_core       # Assumes your core library target is named this
  GTest::gtest_main
)

add_test(NAME MessageParserTests COMMAND test_messageparser)


# -----------------------------------------------------------------------------
# Test Target: test_rendering_performance
# The benchmark test, which requires Qt libraries.
# -----------------------------------------------------------------------------
add_executable(test_rendering_performance test_rendering_performance.cpp)

find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)

# Link against the GUI library and Qt
target_link_libraries(test_rendering_performance PRIVATE
  sentinel_core
  sentinel_gui_lib        # GUI library target
  GTest::gtest_main
  Qt6::Widgets
  Qt6::Charts
)

add_test(NAME RenderingPerformanceTest COMMAND test_rendering_performance)

# You can add more test executables here as you create them...
# add_executable(test_authenticator test_authenticator.cpp)
# ...

message(STATUS "✅ Sentinel test suite configured successfully!")
