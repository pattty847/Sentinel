find_package(Qt6 REQUIRED COMPONENTS Core)

# Test Target: test_log_throttle
add_executable(test_log_throttle test_log_throttle.cpp)
target_include_directories(test_log_throttle PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)
target_link_libraries(test_log_throttle PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME LogThrottleTests COMMAND test_log_throttle)
set_tests_properties(LogThrottleTests PROPERTIES LABELS "core")

# Test Target: test_log_sink (installs the process-wide message handler)
add_executable(test_log_sink test_log_sink.cpp)
target_include_directories(test_log_sink PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)
target_link_libraries(test_log_sink PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME LogSinkTests COMMAND test_log_sink)
set_tests_properties(LogSinkTests PROPERTIES LABELS "core")

# Test Target: test_metrics_registry (registry text format + the /metrics HTTP route)
find_package(Qt6 REQUIRED COMPONENTS Network)
add_executable(test_metrics_registry test_metrics_registry.cpp)
target_include_directories(test_metrics_registry PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
)
target_link_libraries(test_metrics_registry PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
    Qt6::Network
)
add_test(NAME MetricsRegistryTests COMMAND test_metrics_registry)
set_tests_properties(MetricsRegistryTests PROPERTIES LABELS "core")
