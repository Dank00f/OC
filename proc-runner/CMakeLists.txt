cmake_minimum_required(VERSION 3.16)
project(proc_runner LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Куда класть артефакты
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
# Библиотека
add_library(proc_runner STATIC src/proc_runner.cpp)
target_include_directories(proc_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(WIN32)
  target_compile_definitions(proc_runner PRIVATE -DUNICODE -DWIN32_LEAN_AND_MEAN)
endif()

# Тестовая утилита
add_executable(proc_test examples/proc_test.cpp)
target_link_libraries(proc_test PRIVATE proc_runner)
