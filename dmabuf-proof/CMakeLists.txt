cmake_minimum_required(VERSION 3.20)
project(dmabuf_proof LANGUAGES CXX)
find_package(Vulkan REQUIRED)
find_program(GLSLANG glslangValidator REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pattern.spv
    COMMAND ${GLSLANG} -V ${CMAKE_CURRENT_SOURCE_DIR}/pattern.comp -o ${CMAKE_CURRENT_BINARY_DIR}/pattern.spv
    DEPENDS pattern.comp VERBATIM)
add_custom_target(pattern_shader DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/pattern.spv)
add_executable(dmabuf-producer producer.cpp)
add_dependencies(dmabuf-producer pattern_shader)
target_compile_features(dmabuf-producer PRIVATE cxx_std_17)
target_compile_definitions(dmabuf-producer PRIVATE PATTERN_SPV="${CMAKE_CURRENT_BINARY_DIR}/pattern.spv")
target_compile_options(dmabuf-producer PRIVATE -Wall -Wextra)
target_link_libraries(dmabuf-producer PRIVATE Vulkan::Vulkan)
