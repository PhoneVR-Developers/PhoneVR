cmake_minimum_required(VERSION 3.4.3)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -g")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS} -O3 -std=c++17 -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS} -O3 -std=c++17 -g")

set(ANDROID_STL c++_static)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)

add_library(native-lib-alvr SHARED
    src/main/cpp/alvr_main.cpp
)

# Import the ARCore (Google Play Services for AR) library.
add_library(arcore SHARED IMPORTED)
set_target_properties(arcore PROPERTIES IMPORTED_LOCATION
        ${ARCORE_LIBPATH}/${ANDROID_ABI}/libarcore_sdk_c.so
        INTERFACE_INCLUDE_DIRECTORIES ${ARCORE_INCLUDE}
)

set(libs_dir ${CMAKE_CURRENT_SOURCE_DIR}/libraries)
set(alvr_build_dir ${CMAKE_CURRENT_SOURCE_DIR}/../ALVR/build/alvr_client_core)

target_include_directories(native-lib-alvr
    # ALVR Headers
    PUBLIC ${alvr_build_dir}
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../cardboard
    PUBLIC ${ARCORE_INCLUDE}
)

target_link_libraries(native-lib-alvr
    log android EGL GLESv3 mediandk arcore
    ${alvr_build_dir}/${ANDROID_ABI}/libalvr_client_core.so
    ${libs_dir}/jni/${ANDROID_ABI}/libGfxPluginCardboard.so
)
