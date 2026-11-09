find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(test_flow_bar test_flow_bar.cpp)
target_link_libraries(test_flow_bar PRIVATE sentinel_core GTest::gtest_main)
add_test(NAME FlowBarTests COMMAND test_flow_bar)
set_tests_properties(FlowBarTests PROPERTIES LABELS "core")

add_executable(flow_store_size_bench EXCLUDE_FROM_ALL flow_store_size_bench.cpp)
target_link_libraries(flow_store_size_bench PRIVATE sentinel_core)

add_test(NAME DeployRuntimeDryRun
    COMMAND bash ${CMAKE_SOURCE_DIR}/tests/test_deploy_runtime_dry_run.sh
            ${CMAKE_SOURCE_DIR}/scripts/dev/deploy-runtime.sh)
set_tests_properties(DeployRuntimeDryRun PROPERTIES LABELS "core")

add_test(NAME AgentWorktreeLandTests
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/dev/test_agent_worktree.sh ${CMAKE_SOURCE_DIR})
set_tests_properties(AgentWorktreeLandTests PROPERTIES LABELS "core" TIMEOUT 300)

add_test(NAME DocHealthTests
    COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/dev/test_doc_health.sh ${CMAKE_SOURCE_DIR})
set_tests_properties(DocHealthTests PROPERTIES LABELS "core" TIMEOUT 60)

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

# Test Target: test_price_display_unit (scaled display unit for tiny-priced products)
add_executable(test_price_display_unit test_price_display_unit.cpp)
target_include_directories(test_price_display_unit PRIVATE ${CMAKE_SOURCE_DIR}/libs/core)
target_link_libraries(test_price_display_unit PRIVATE GTest::gtest_main)
add_test(NAME PriceDisplayUnitTests COMMAND test_price_display_unit)
set_tests_properties(PriceDisplayUnitTests PROPERTIES LABELS "core")
