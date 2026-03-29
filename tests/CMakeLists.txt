# =============================================================================
# Sentinel Test Suite - CMakeLists.txt
# Unit tests for market data processing pipeline
# =============================================================================

enable_testing()

# Market data tests are in their own subdirectory
# These tests validate the V2 architecture:
# - MessageDispatcher: Channel-based message routing
# - SubscriptionManager: Symbol subscription lifecycle
add_subdirectory(marketdata)
add_subdirectory(render)

message(STATUS "Marketdata tests configured (3 test suites)")
message(STATUS "Render tests configured (2 test suites)")

add_subdirectory(trading)
message(STATUS "Trading tests configured")
