cmake_minimum_required(VERSION 3.5)

project(slimmemeter LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)

find_library(RRD_LIBRARY librrd.so HINTS /usr/lib/arm-linux-gnueabihf)
add_executable(slimmemeter slimmemeter.c slimmemeter.h)
target_link_libraries(slimmemeter PUBLIC ${RRD_LIBRARY})