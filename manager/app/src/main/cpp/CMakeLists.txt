# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.
cmake_minimum_required(VERSION 3.18.1)

# uapi headers: resolve repo root relative to this CMakeLists.txt
# CMAKE_CURRENT_SOURCE_DIR = manager/app/src/main/cpp/
# Going up 5 levels reaches the repo root
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
include_directories(${REPO_ROOT})

project("kernelsu")

include("${REPO_ROOT}/cmake/ClangTidy.cmake")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Supplied by the cmaker plugin for release symbol collection. Referencing the
# value here keeps CMake's command-line variable validation accurate.
if(DEFINED DEBUG_SYMBOLS_PATH)
    message(VERBOSE "Native debug symbols path: ${DEBUG_SYMBOLS_PATH}")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "ZySU Manager supports Android arm64-v8a only")
endif()

find_package(cxx REQUIRED CONFIG)
link_libraries(cxx::cxx)

add_library(kernelsu
        SHARED
        jni.c
        ksud_integrity.c
        sumhp_jni.cpp
        ${REPO_ROOT}/userspace/common/maps_preview_transport.cpp
        ${REPO_ROOT}/userspace/common/profile_manager_transport.cpp
        ${REPO_ROOT}/userspace/ksud/sumhp/src/mount/sumh_client.cpp
        ksu.c
)
target_include_directories(kernelsu PRIVATE ${REPO_ROOT}/userspace/ksud/sumhp/include)
target_compile_definitions(kernelsu PRIVATE SUMHP_EMBEDDED)
set_source_files_properties(sumhp_jni.cpp
        ${REPO_ROOT}/userspace/common/maps_preview_transport.cpp
        ${REPO_ROOT}/userspace/common/profile_manager_transport.cpp
        ${REPO_ROOT}/userspace/ksud/sumhp/src/mount/sumh_client.cpp
        PROPERTIES COMPILE_OPTIONS "-fexceptions;-funwind-tables")

find_library(log-lib log)

# The control bridge needs streams and exception support omitted by the compact
# libcxx prefab. Keep its full static runtime private to this JNI library.
set_property(TARGET kernelsu PROPERTY LINK_LIBRARIES "")
target_include_directories(kernelsu SYSTEM PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${CMAKE_SYSROOT}/usr/include/c++/v1>)
target_link_libraries(kernelsu ${log-lib}
        ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/libc++_static.a
        ${CMAKE_SYSROOT}/usr/lib/aarch64-linux-android/libc++abi.a)

add_library(adbroot
        SHARED
        adbroot.cpp
)

target_link_libraries(adbroot ${log-lib})

zysu_enable_clang_tidy(kernelsu)
zysu_enable_clang_tidy(adbroot CXX)
