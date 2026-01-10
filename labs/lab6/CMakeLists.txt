cmake_minimum_required(VERSION 3.16)
project(lab6_temp_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(temp_server
    src/temp_server.cpp
)

# GUI собираем только если найден Qt6
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 QUIET COMPONENTS Widgets Network)

if (Qt6_FOUND)
    add_executable(temp_gui
        src/main.cpp
    )
    target_link_libraries(temp_gui PRIVATE Qt6::Widgets Qt6::Network)
else()
    message(WARNING "Qt6 not found: temp_gui will NOT be built. Install Qt6 and reconfigure.")
endif()

if (WIN32)
    target_compile_definitions(temp_server PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
