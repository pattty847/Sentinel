# =============================================================================
# SENTINEL TESTS - CMakeLists.txt
# Defines test executables for unit, integration, and performance testing.
# =============================================================================

# This allows CTest to discover and run the tests defined here.
enable_testing()

# Find Qt components needed for GUI-based tests
find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)

# Set common properties for Qt-based test executables
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# -----------------------------------------------------------------------------
# Test Target: test_messageparser
# A standalone unit test for the JSON parser. It has no Qt dependencies.
# -----------------------------------------------------------------------------
add_executable(test_messageparser test_messageparser.cpp)

# Link against the core library for data structures, GTest for testing,
# and nlohmann_json for the exception handling.
target_link_libraries(test_messageparser PRIVATE
  sentinel_core
  GTest::gtest_main
  nlohmann_json::nlohmann_json
)

add_test(NAME MessageParserTests COMMAND test_messageparser)

# -----------------------------------------------------------------------------
# Test Target: test_rendering_performance
# A GTest-based benchmark for comparing QPainter vs. GPU rendering.
# -----------------------------------------------------------------------------
add_executable(test_rendering_performance test_rendering_performance.cpp)

# Link against the GUI library and Qt
target_link_libraries(test_rendering_performance PRIVATE
  sentinel_core
  sentinel_gui_lib
  GTest::gtest_main
  Qt6::Widgets
  Qt6::Charts
)

add_test(NAME RenderingPerformanceTest COMMAND test_rendering_performance)

# -----------------------------------------------------------------------------
# Executable Target: test_performance_baseline
# A standalone benchmark tool to measure pre-optimization performance.
# This is not a GTest test and will not be run by CTest.
# -----------------------------------------------------------------------------
# add_executable(test_performance_baseline test_performance_baseline.cpp)

# target_link_libraries(test_performance_baseline PRIVATE
#   sentinel_core
#   sentinel_gui_lib
#   Qt6::Widgets
#   Qt6::Charts
# )

message(STATUS "✅ Sentinel test suite configured with 2 targets.")
