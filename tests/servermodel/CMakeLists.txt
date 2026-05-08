find_package(Qt6 REQUIRED COMPONENTS Core)

add_executable(test_hmcol_format test_hmcol_format.cpp)
target_include_directories(test_hmcol_format PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/core
    ${CMAKE_SOURCE_DIR}/tests
)
target_link_libraries(test_hmcol_format PRIVATE
    sentinel_core
    GTest::gtest_main
    Qt6::Core
)
add_test(NAME HmcolFormatTests COMMAND test_hmcol_format)
set_tests_properties(HmcolFormatTests PROPERTIES LABELS "servermodel")

add_custom_target(servermodel_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L servermodel
    DEPENDS test_hmcol_format
    COMMENT "Running servermodel persistence tests"
)

message(STATUS "Servermodel tests configured (1 test suite)")
