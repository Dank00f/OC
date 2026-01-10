cmake_minimum_required(VERSION 3.16)
project(lab5_temp_server LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(temp_logger src/temp_logger.cpp)

find_package(Threads REQUIRED)
target_link_libraries(temp_logger PRIVATE Threads::Threads)

if (WIN32)
  target_compile_definitions(temp_logger PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(temp_logger PRIVATE ws2_32)
endif()

# --- SQLite amalgamation (LOCAL, no system packages) ---
set(SQLITE_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite")
if (EXISTS "${SQLITE_DIR}/sqlite3.c" AND EXISTS "${SQLITE_DIR}/sqlite3.h")
  add_library(sqlite3_amalg STATIC "${SQLITE_DIR}/sqlite3.c")
  target_include_directories(sqlite3_amalg PUBLIC "${SQLITE_DIR}")
  if (WIN32)
    target_compile_definitions(sqlite3_amalg PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  target_link_libraries(temp_logger PRIVATE sqlite3_amalg)
else()
  message(FATAL_ERROR "Put sqlite3.c/sqlite3.h into third_party/sqlite/")
endif()
