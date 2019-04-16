cmake_minimum_required(VERSION 3.4.1)

add_library(akono-jni SHARED
            akono-jni.c)

add_library(v8 STATIC IMPORTED)
set_target_properties(v8 PROPERTIES IMPORTED_LOCATION
    ${distribution_DIR}/gperf/lib/${ANDROID_ABI}/libgperf.so)


# Include libraries needed for hello-jni lib
target_link_libraries(akono-jni
                      android
                      log)

