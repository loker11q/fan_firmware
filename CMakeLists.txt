cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/Application"
    "${CMAKE_CURRENT_LIST_DIR}/Services"
    "${CMAKE_CURRENT_LIST_DIR}/Hardware"
)
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_bldc_fan)
