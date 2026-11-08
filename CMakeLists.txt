# ESP-IDF project file (PlatformIO runs ESP-IDF's CMake build through it).
cmake_minimum_required(VERSION 3.16.0)

# Firmware version: taken from include/config.h (FW_VERSION). It ends up in the image's app description.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/include/config.h" _version_line REGEX "^#define FW_VERSION ")
string(REGEX REPLACE "^#define FW_VERSION +\"([^\"]+)\".*$" "\\1" PROJECT_VER "${_version_line}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/include/config.h")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(franke_can)
