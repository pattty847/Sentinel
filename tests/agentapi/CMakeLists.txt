add_executable(test_AgentApiCodec
    test_AgentApiCodec.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/render/heatmap/HeatmapSettingsStore.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow/AgentApiCodec.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow/AgentApiOperations.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow/AgentApiSnapshots.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/datasources/CandleSeriesBuffer.cpp
)
target_include_directories(test_AgentApiCodec PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui/mainwindow ${CMAKE_SOURCE_DIR}/libs/core ${CMAKE_SOURCE_DIR}/libs/gui/datasources)
target_link_libraries(test_AgentApiCodec PRIVATE GTest::gtest_main Qt6::Core sentinel_core)
add_test(NAME AgentApiCodecTests COMMAND test_AgentApiCodec)
set_tests_properties(AgentApiCodecTests PROPERTIES LABELS "agentapi")

add_executable(test_AgentApiInput test_AgentApiInput.cpp)
target_include_directories(test_AgentApiInput PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_AgentApiInput PRIVATE sentinel_gui_lib GTest::gtest Qt6::Quick Qt6::Network)
add_test(NAME AgentApiInputTests COMMAND test_AgentApiInput)
set_tests_properties(AgentApiInputTests PROPERTIES LABELS "agentapi" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_AgentApiDocks test_AgentApiDocks.cpp)
target_include_directories(test_AgentApiDocks PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_link_libraries(test_AgentApiDocks PRIVATE sentinel_gui_lib GTest::gtest Qt6::Widgets Qt6::Network)
add_test(NAME AgentApiDocksTests COMMAND test_AgentApiDocks)
set_tests_properties(AgentApiDocksTests PROPERTIES LABELS "agentapi" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(test_AgentHostMode
    test_AgentHostMode.cpp
    ${CMAKE_SOURCE_DIR}/libs/gui/config/AgentHostMode.cpp
)
target_include_directories(test_AgentHostMode PRIVATE ${CMAKE_SOURCE_DIR}/libs/gui)
target_compile_definitions(test_AgentHostMode PRIVATE SENTINEL_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(test_AgentHostMode PRIVATE GTest::gtest_main Qt6::Core)
add_test(NAME AgentHostModeTests COMMAND test_AgentHostMode)
set_tests_properties(AgentHostModeTests PROPERTIES LABELS "agentapi")
