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
