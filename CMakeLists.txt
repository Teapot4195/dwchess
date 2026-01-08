cmake_minimum_required(VERSION 4.1)
project(dwchess)

set(CMAKE_CXX_STANDARD 26)

add_executable(dwchess
    src/main.cpp
    lib/chess.h
)
target_include_directories(dwchess PRIVATE lib)
