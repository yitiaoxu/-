cmake_minimum_required(VERSION 3.11)
project(rk3588-vision-qt VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIB_ARCH "aarch64")

set(RKNN_API_PATH ${CMAKE_CURRENT_SOURCE_DIR}/librknn_api)
set(RKNN_API_INCLUDE_PATH ${RKNN_API_PATH}/include)
set(RKNN_API_LIB_PATH ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)
if(NOT EXISTS "${RKNN_API_LIB_PATH}")
    message(FATAL_ERROR "Missing RKNN runtime: ${RKNN_API_LIB_PATH}\n"
        "Copy librknnrt.so to librknn_api/aarch64/ on the board.")
endif()

find_package(OpenCV REQUIRED)
message(STATUS "OpenCV ${OpenCV_VERSION}")

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt5 COMPONENTS Core Widgets Gui REQUIRED)

find_package(OpenCV QUIET COMPONENTS freetype)
add_library(plate_rknn_pipeline STATIC src/plate/plate_rknn_pipeline.cpp)
target_include_directories(plate_rknn_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
    ${RKNN_API_INCLUDE_PATH}
)
if(OpenCV_freetype_FOUND)
    target_compile_definitions(plate_rknn_pipeline PUBLIC USE_OPENCV_FREETYPE=1)
    message(STATUS "Chinese labels: ENABLED (opencv_freetype)")
else()
    message(STATUS "Chinese labels: DISABLED")
endif()
target_link_libraries(plate_rknn_pipeline PUBLIC
    ${OpenCV_LIBS}
    pthread
    ${RKNN_API_LIB_PATH}
)
if(OpenCV_freetype_FOUND)
    target_link_libraries(plate_rknn_pipeline PUBLIC opencv_freetype)
endif()

add_executable(vision_qt_demo
    src/qt0323/main.cpp
    src/qt0323/mainwindow.cpp
    src/qt0323/qt_resources.qrc
)
target_include_directories(vision_qt_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(vision_qt_demo PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
target_link_libraries(vision_qt_demo
    Qt5::Core
    Qt5::Widgets
    Qt5::Gui
    plate_rknn_pipeline
    pthread
)

add_subdirectory(test_rknn_infer)

message(STATUS "RKNN_API_LIB_PATH=${RKNN_API_LIB_PATH}")
