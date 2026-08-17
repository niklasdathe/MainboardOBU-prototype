cmake_minimum_required(VERSION 3.22)

# The direct S3 -> Wi-Fi/hotspot -> OpenTrafficMap uplink is parked in issue #6.
# Keep components/obu_otm in the repository for later continuation, but do not
# compile it into normal S3 firmware builds.
set(EXCLUDE_COMPONENTS obu_otm)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bicycleobu_s3 VERSION 0.1.0)
