cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Use custom partition table
set(PARTITION_CSV_PATH ${CMAKE_SOURCE_DIR}/partitions.csv)

project(oled_mp3_player)