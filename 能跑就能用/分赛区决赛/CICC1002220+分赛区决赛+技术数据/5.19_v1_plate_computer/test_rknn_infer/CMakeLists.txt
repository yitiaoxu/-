# CLI validator ¡ª links parent plate_rknn_pipeline (see root CMakeLists.txt)
add_executable(rknn_infer_one rknn_infer_one.cpp)
target_include_directories(rknn_infer_one PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(rknn_infer_one PRIVATE plate_rknn_pipeline)
