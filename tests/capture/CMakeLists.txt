add_executable(test_raw_capture test_raw_capture.cpp)
target_include_directories(test_raw_capture PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(test_raw_capture PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
add_test(NAME RawCaptureTests COMMAND test_raw_capture)
set_tests_properties(RawCaptureTests PROPERTIES LABELS "capture" TIMEOUT 60)

add_executable(capture_app_fixture capture_app_fixture.cpp)
target_include_directories(capture_app_fixture PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(capture_app_fixture PRIVATE sentinel_core Qt6::Core)
add_executable(test_capture_app test_capture_app.cpp)
target_link_libraries(test_capture_app PRIVATE sentinel_core GTest::gtest_main Qt6::Core)
target_compile_definitions(test_capture_app PRIVATE
    CAPTURE_APP_FIXTURE="$<TARGET_FILE:capture_app_fixture>"
    SENTINEL_TEST_CA="${CMAKE_SOURCE_DIR}/resources/certs/ca-bundle.crt")
add_dependencies(test_capture_app capture_app_fixture)
add_test(NAME CaptureApplicationTests COMMAND test_capture_app)
set_tests_properties(CaptureApplicationTests PROPERTIES LABELS "capture" TIMEOUT 30)
