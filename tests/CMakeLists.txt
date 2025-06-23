# =============================================================================
# SENTINEL TESTS - CMakeLists.txt
# Organized test suite for the revolutionary refactored architecture
# =============================================================================

cmake_minimum_required(VERSION 3.22)

# Test executables with proper include directories
add_executable(test_comprehensive test_comprehensive.cpp)
target_link_libraries(test_comprehensive PRIVATE sentinel_core)
target_include_directories(test_comprehensive PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(test_datacache test_datacache.cpp)
target_link_libraries(test_datacache PRIVATE sentinel_core)
target_include_directories(test_datacache PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(test_facade test_facade.cpp)
target_link_libraries(test_facade PRIVATE sentinel_core)
target_include_directories(test_facade PRIVATE ${CMAKE_SOURCE_DIR})

# Set C++17 standard for all tests
set_property(TARGET test_comprehensive PROPERTY CXX_STANDARD 17)
set_property(TARGET test_datacache PROPERTY CXX_STANDARD 17)
set_property(TARGET test_facade PROPERTY CXX_STANDARD 17)

# Add test descriptions
set_target_properties(test_comprehensive PROPERTIES DESCRIPTION "Phase 6 comprehensive test suite")
set_target_properties(test_datacache PROPERTIES DESCRIPTION "Phase 3 DataCache validation tests")
set_target_properties(test_facade PROPERTIES DESCRIPTION "Phase 5 Facade pattern tests")

message(STATUS "✅ Sentinel test suite configured successfully!")
message(STATUS "   - test_comprehensive: Full architectural validation")
message(STATUS "   - test_datacache: DataCache performance & thread safety")
message(STATUS "   - test_facade: Facade pattern integration tests") 