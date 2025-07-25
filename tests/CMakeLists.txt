# =============================================================================
# SENTINEL COMPREHENSIVE TEST SUITE - CMakeLists.txt
# Professional-grade testing for hedge fund / institutional use
# =============================================================================

# This allows CTest to discover and run the tests defined here.
enable_testing()

# Find Qt components needed for GUI-based tests
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Charts Network Test)

# Set common properties for Qt-based test executables
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Common libraries for all tests
set(COMMON_TEST_LIBS
    sentinel_core
    sentinel_gui_lib
    GTest::gtest_main
    Qt6::Core
    Qt6::Widgets
    Qt6::Charts
    Qt6::Network
    Qt6::Test
)

# =============================================================================
# CORE UNIT TESTS
# =============================================================================

# Test Target: test_messageparser
add_executable(test_messageparser test_messageparser.cpp)
target_link_libraries(test_messageparser PRIVATE
    sentinel_core
    GTest::gtest_main
    nlohmann_json::nlohmann_json
)
add_test(NAME MessageParserTests COMMAND test_messageparser)

# Test Target: test_coordinate_system
add_executable(test_coordinate_system test_coordinate_system.cpp)
target_link_libraries(test_coordinate_system PRIVATE ${COMMON_TEST_LIBS})
add_test(NAME CoordinateSystemTest COMMAND test_coordinate_system)

# =============================================================================
# PROFESSIONAL PERFORMANCE TESTING SUITE
# =============================================================================

# Test Target: test_data_integrity
# Gap detection, state validation, temporal continuity
add_executable(test_data_integrity test_data_integrity.cpp)
target_link_libraries(test_data_integrity PRIVATE ${COMMON_TEST_LIBS})
add_test(NAME DataIntegrityTest COMMAND test_data_integrity)

# Test Target: test_stress_performance  
# Verifies claimed metrics: 32x memory reduction, 2.27M trades/sec, firehose handling
add_executable(test_stress_performance test_stress_performance.cpp)
target_link_libraries(test_stress_performance PRIVATE ${COMMON_TEST_LIBS})
add_test(NAME StressPerformanceTest COMMAND test_stress_performance)

# Test Target: test_professional_requirements
# Bloomberg terminal parity, anti-spoofing, disaster recovery, compliance
add_executable(test_professional_requirements test_professional_requirements.cpp)
target_link_libraries(test_professional_requirements PRIVATE ${COMMON_TEST_LIBS})
add_test(NAME ProfessionalRequirementsTest COMMAND test_professional_requirements)

# Test Target: test_order_book_state
# L2 message consistency, deep order book management, state integrity
add_executable(test_order_book_state test_order_book_state.cpp)
target_link_libraries(test_order_book_state PRIVATE ${COMMON_TEST_LIBS})
add_test(NAME OrderBookStateTest COMMAND test_order_book_state)

# =============================================================================
# LEGACY PERFORMANCE TESTS (For comparison)
# =============================================================================

# Test Target: test_rendering_performance (Legacy)
# add_executable(test_rendering_performance test_rendering_performance.cpp)
# target_link_libraries(test_rendering_performance PRIVATE ${COMMON_TEST_LIBS})
# add_test(NAME RenderingPerformanceTest COMMAND test_rendering_performance)

# =============================================================================
# COMPREHENSIVE TEST SUITE RUNNER
# =============================================================================

# Custom target to run all professional tests
add_custom_target(professional_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --verbose
    DEPENDS
        test_data_integrity
        test_stress_performance
        test_professional_requirements
        test_order_book_state
    COMMENT "Running comprehensive professional test suite"
)

# Custom target to run performance validation
add_custom_target(verify_metrics
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "StressPerformanceTest|DataIntegrityTest"
    DEPENDS
        test_stress_performance
        test_data_integrity
    COMMENT "Verifying claimed performance metrics"
)

# Custom target for hedge fund grade validation
add_custom_target(hedge_fund_validation
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -R "ProfessionalRequirementsTest|OrderBookStateTest"
    DEPENDS
        test_professional_requirements
        test_order_book_state
    COMMENT "Running hedge fund grade validation tests"
)

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