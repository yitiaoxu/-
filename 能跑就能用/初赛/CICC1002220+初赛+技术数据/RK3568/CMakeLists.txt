# ??????????????
cmake_minimum_required(VERSION 3.11 FATAL_ERROR)
# ????????
project(rk3588-vision-qt VERSION 0.0.1 LANGUAGES CXX)

# ??????????
message(STATUS "System: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}")

# ?????????set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")

# ??????????set(LIB_ARCH "aarch64")
set(LIB_ARCH "aarch64")
set(DEVICE_NAME "RK3568")

set(3RDPARTY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty)
#set(RGA_DIR ${3RDPARTY_PATH}/rga/${DEVICE_NAME})
#set(RGA_LIB ${RGA_DIR}/lib/Linux/${LIB_ARCH}/librga.so)
find_library(RGA_LIB NAMES rga)

link_directories(
    ${RGA_DIR}/lib/Linux/${LIB_ARCH}
)


set(RKNN_API_PATH ${CMAKE_CURRENT_SOURCE_DIR}/librknn_api)
set(RKNN_API_INCLUDE_PATH ${RKNN_API_PATH}/include)
set(RKNN_API_LIB_PATH ${RKNN_API_PATH}/${LIB_ARCH}/librknnrt.so)

find_package(OpenCV REQUIRED)
message(STATUS "OpenCV include path: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV version: ${OpenCV_VERSION}")


# ????Qt ???MOC, UIC, RCC ??????????????????
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ???? Qt5 ????????UI ???
find_package(Qt5 COMPONENTS Core Widgets Gui REQUIRED)


include_directories(
    /usr/include/eigen3
    ${OpenCV_INCLUDE_DIRS}
    ${RKNN_API_INCLUDE_PATH}
    ${RGA_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/task
    ${CMAKE_CURRENT_SOURCE_DIR}/src/draw
    ${CMAKE_CURRENT_SOURCE_DIR}/src/qt0323       
)

add_library(nn_process SHARED 
    src/process/preprocess.cpp 
    src/process/postprocess.cpp
    src/process/preprocess_v11.cpp
    src/process/postprocess_v11.cpp
)

# ? ?????????????? im2d.h ????
target_include_directories(nn_process PRIVATE
    /usr/include/rga
)

target_link_libraries(nn_process
    ${OpenCV_LIBS}
    ${RGA_LIB}
)


add_library(rknn_engine SHARED src/engine/rknn_engine.cpp)

target_link_libraries(rknn_engine
    ${RKNN_API_LIB_PATH}
)


add_library(yolov11_lib SHARED src/task/yolov11_custom.cpp)
target_link_libraries(yolov11_lib rknn_engine nn_process)

add_library(draw_lib SHARED src/draw/cv_draw.cpp)
target_link_libraries(draw_lib ${OpenCV_LIBS})


add_executable(vision_qt_demo
        src/qt0323/main.cpp
        src/qt0323/lprnet.cpp
        src/qt0323/mainwindow.cpp
        src/task/yolov11_thread_pool.cpp   
)

target_link_libraries(vision_qt_demo
        Qt5::Core
        Qt5::Widgets
        Qt5::Gui
        ${OpenCV_LIBS}
        yolov11_lib
        draw_lib
        pthread
)