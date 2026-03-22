# Render system tests — TimeAxisMapping

find_package(Qt6 REQUIRED COMPONENTS Core Gui)

add_executable(test_TimeAxisMapping test_TimeAxisMapping.cpp)
target_include_directories(test_TimeAxisMapping PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_TimeAxisMapping PRIVATE
    GTest::gtest_main
    Qt6::Core
    Qt6::Gui
)
add_test(NAME TimeAxisMappingTests COMMAND test_TimeAxisMapping)
set_tests_properties(TimeAxisMappingTests PROPERTIES LABELS "render")

add_executable(test_UgrFrameMath
    test_UgrFrameMath.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/UgrFrameMath.cpp
)
target_include_directories(test_UgrFrameMath PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_UgrFrameMath PRIVATE
    GTest::gtest_main
    Qt6::Core
    Qt6::Gui
)
add_test(NAME UgrFrameMathTests COMMAND test_UgrFrameMath)
set_tests_properties(UgrFrameMathTests PROPERTIES LABELS "render")

add_executable(test_AxisLayout test_AxisLayout.cpp)
target_include_directories(test_AxisLayout PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/gui
)
target_link_libraries(test_AxisLayout PRIVATE
    GTest::gtest_main
    sentinel_gui_lib
    Qt6::Core
    Qt6::Gui
)
target_compile_definitions(test_AxisLayout PRIVATE
    SENTINEL_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
add_test(NAME AxisLayoutTests COMMAND test_AxisLayout)
set_tests_properties(AxisLayoutTests PROPERTIES LABELS "render")
