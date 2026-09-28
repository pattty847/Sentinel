add_executable(test_AgentApiCodec
    test_AgentApiCodec.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow/AgentApiCodec.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow/AgentApiSnapshots.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/datasources/CandleSeriesBuffer.cpp
)
target_include_directories(test_AgentApiCodec PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/libs/gui/datasources)
target_link_libraries(test_AgentApiCodec PRIVATE GTest::gtest_main Qt6::Core sentinel_core)
add_test(NAME AgentApiCodecTests COMMAND test_AgentApiCodec)
set_tests_properties(AgentApiCodecTests PROPERTIES LABELS "agentapi")
