# CMakeLists.txt
# This file is used to build the Sentinel application.
# It is used to configure the build environment and the dependencies.
# It is used to build the application.

# Set the minimum required CMake version
cmake_minimum_required(VERSION 3.16)
project(Sentinel LANGUAGES CXX)

# Set the C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable Qt's automatic tools for Qt6
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find the Qt6 libraries
find_package(Qt6 REQUIRED COMPONENTS Widgets WebSockets)
find_package(Boost COMPONENTS system thread)
if(Boost_FOUND)
    message(STATUS "Using Boost")
else()
    add_definitions(-DASIO_STANDALONE)
endif()

# --- Group Source and Header Files ---
# This makes the file cleaner and easier to manage.

# Source files (.cpp) that need to be compiled
set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/websocketclient.cpp
    src/coinbasestreamclient.cpp
    src/statisticsprocessor.cpp
    src/statisticscontroller.cpp
    src/ruleengine.cpp
    src/cvdthresholdrule.cpp
)

# Header files (.h). Including them ensures they appear in IDEs 
# and that AUTOMOC can process any headers with Q_OBJECT.
set(HEADERS
    src/mainwindow.h
    src/websocketclient.h
    src/coinbasestreamclient.h
    src/statisticsprocessor.h
    src/statisticscontroller.h
    src/ruleengine.h
    src/cvdthresholdrule.h
    src/rule.h
    src/tradedata.h
)

# Add our source and header files to the executable
add_executable(sentinel 
    ${SOURCES}
    ${HEADERS}
)

# Link our executable against the Qt libraries
target_link_libraries(sentinel PRIVATE
    Qt6::Widgets
    Qt6::WebSockets
    Boost::system
    Boost::thread
)

add_executable(coinbase_stream_test
    src/stream_main.cpp
    src/coinbasestreamclient.cpp
    src/coinbasestreamclient.h
)
target_link_libraries(coinbase_stream_test PRIVATE
    Boost::system
    Boost::thread
)
