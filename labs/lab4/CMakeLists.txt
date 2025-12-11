cmake_minimum_required(VERSION 3.10)
project(temp_logger_proj CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(temp_logger src/temp_logger.cpp)
add_executable(temp_sim    src/temp_sim.cpp)
