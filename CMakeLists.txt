# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# 设置额外的组件目录，告诉ESP-IDF在哪里找到esp-dl和模型组件
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-dl/esp-dl"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/esp-dl/models/imagenet_cls"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 配置分区表文件
set(PARTITION_TABLE_FILENAME "partitions.csv")

project(CAM_AI)