cmake_minimum_required(VERSION 3.14)
project(ksuinit VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "ZySU userspace supports Android arm64-v8a only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use ccache if available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ClangTidy.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NativeToolchainMetadata.cmake")

# Size optimization (Os + LTO).
# ksuinit is injected as /init into early ramdisk, so it must be fully static.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden -fno-rtti -fno-exceptions -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident -faddrsig -fno-semantic-interposition")
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG -flto=full")
set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG -flto=full")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -fuse-ld=lld -flto=full -Wl,--gc-sections -Wl,--icf=all -Wl,--exclude-libs,ALL -Wl,-O2 -Wl,--lto-O3 -Wl,--hash-style=gnu")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/NativeHardening.cmake")

get_filename_component(REPO_ROOT "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(ksuinit
    src/main.cpp
    src/init.cpp
    src/loader.cpp
    src/log.cpp
    src/module_symbols.cpp
    src/vermagic.cpp
    ${REPO_ROOT}/userspace/common/cxx_slim.cpp
)

target_include_directories(ksuinit PRIVATE src ${REPO_ROOT})
zysu_enable_clang_tidy(ksuinit CXX)
zysu_finalize_native(ksuinit static)

# Keep the old cache option to diagnose unsupported packed builds explicitly.
option(KSUINIT_UPX "Use UPX to compress ksuinit binary" OFF)
if(KSUINIT_UPX)
    message(FATAL_ERROR "KSUINIT_UPX is incompatible with the verified ELF/symbol artifacts; configure with KSUINIT_UPX=OFF")
endif()

# No external dependencies - pure C++ with POSIX/Linux APIs
zysu_record_native_toolchain(ksuinit)
