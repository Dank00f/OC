cmake_minimum_required(VERSION 3.10)
project(proc_runner_lab3 LANGUAGES CXX)

add_library(proc_runner STATIC)

if (WIN32)
    target_sources(proc_runner PRIVATE src/proc_runner_win.cpp)
else()
    target_sources(proc_runner PRIVATE src/proc_runner_posix.cpp)
endif()

target_include_directories(proc_runner PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(proc_runner PUBLIC cxx_std_17)

add_executable(proc_test tests/proc_test.cpp)
target_link_libraries(proc_test PRIVATE proc_runner)
