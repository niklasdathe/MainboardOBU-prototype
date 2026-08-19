cmake_minimum_required(VERSION 3.22)

# The Wio-SX1262 uplink is owned by the ESP32-S3 hub only.
set(EXCLUDE_COMPONENTS obu_lorawan)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bicycleobu_c5_tx VERSION 0.1.0)
