# =============================================================================
# Sentinel Test Suite - CMakeLists.txt
# Unit tests for market data processing pipeline
# =============================================================================

enable_testing()

# Market data tests are in their own subdirectory
# These tests validate the V2 architecture:
# - MessageDispatcher: Channel-based message routing
# - SubscriptionManager: Symbol subscription lifecycle
# - DataCache: Sink adapter integration
add_subdirectory(marketdata)

message(STATUS "Marketdata tests configured (3 test suites)")
