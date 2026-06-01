cmake_minimum_required(VERSION 3.16)

project(esp32_geophone_filter)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_adc freertos
)
