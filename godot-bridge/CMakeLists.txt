cmake_minimum_required(VERSION 3.20)
project(dwl_godot_bridge LANGUAGES C CXX)
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/build_profile.json" CACHE PATH "" FORCE)
set(GODOTCPP_DISABLE_EXCEPTIONS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/godot-cpp)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)
add_library(dwl_bridge SHARED src/bridge_receiver.cpp src/register_types.cpp src/vulkan_probe.cpp src/dmabuf_receiver.cpp src/session_process.cpp)
target_compile_features(dwl_bridge PRIVATE cxx_std_17)
target_compile_definitions(dwl_bridge PRIVATE _GNU_SOURCE)
target_compile_options(dwl_bridge PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dwl_bridge PRIVATE godot-cpp Threads::Threads Vulkan::Vulkan)
set_target_properties(dwl_bridge PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/demo/bin")
add_executable(bridge_session_host src/session_host.c)
target_compile_definitions(bridge_session_host PRIVATE _GNU_SOURCE)
target_compile_options(bridge_session_host PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(bridge_session_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/world/bin")
add_custom_command(TARGET dwl_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/world/bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:dwl_bridge>
        "${CMAKE_CURRENT_SOURCE_DIR}/world/bin/$<TARGET_FILE_NAME:dwl_bridge>")
