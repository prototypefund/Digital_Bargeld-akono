cmake_minimum_required(VERSION 3.4.1)

add_library(
        akono-jni SHARED
        akono-jni.cpp
)

set(deps_dir ${CMAKE_CURRENT_SOURCE_DIR}/../../../../deps)

if(NOT EXISTS ${deps_dir})
    message( FATAL_ERROR "Dependency directory does not exist")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++14")

include_directories(
        ${deps_dir}/android-node-v8/src
        ${deps_dir}/android-node-v8/deps/v8/include
        ${deps_dir}/android-node-v8/deps/uv/include
)

add_library(node SHARED IMPORTED)
set_target_properties(node PROPERTIES IMPORTED_LOCATION
        ${deps_dir}/compiled/${ANDROID_ABI}/libnode.so)

add_library(v8 STATIC IMPORTED)
set_target_properties(v8 PROPERTIES IMPORTED_LOCATION
        ${deps_dir}/compiled/${ANDROID_ABI}/libv8.cr.so)

add_library(v8_platform STATIC IMPORTED)
set_target_properties(v8_platform PROPERTIES IMPORTED_LOCATION
        ${deps_dir}/compiled/${ANDROID_ABI}/libv8_libplatform.cr.so)

# Include libraries needed for hello-jni lib
target_link_libraries(akono-jni
        v8
        v8_platform
        node
        android
        log)


