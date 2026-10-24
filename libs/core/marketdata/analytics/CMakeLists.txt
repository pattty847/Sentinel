add_library(sentinel_book_analytics STATIC
    LeanGauge.hpp LeanGauge.cpp
    BookAnalyticsTypes.hpp
    WindowState.hpp WindowState.cpp
    BookDiffClassifier.hpp BookDiffClassifier.cpp
    LevelTracker.hpp LevelTracker.cpp
    BookHistoryRing.hpp BookHistoryRing.cpp
)
target_include_directories(sentinel_book_analytics PUBLIC ${CMAKE_SOURCE_DIR}/libs/core)
set_target_properties(sentinel_book_analytics PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
