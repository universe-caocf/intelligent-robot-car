# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# 单板型工程默认使用 ESP32-S3；显式选择其他芯片时拒绝构建。
set(IDF_TARGET "esp32s3" CACHE STRING "Target chip")
set(PROJECT_VER "2.1.0")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(NOT IDF_TARGET STREQUAL "esp32s3")
    message(FATAL_ERROR "Intelligent Robot Car only supports ESP32-S3")
endif()
project(intelligent_robot_car)

