# Shared by the Android ARM64 executables. Keep this before add_subdirectory so
# embedded C/C++ libraries receive the same protections as first-party objects.
include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android" OR
   NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a" OR
   NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Native hardening requires the supported Android ARM64 Clang toolchain")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Native artifact checks require a single-configuration generator (for example Ninja)")
endif()
set(_zysu_c_or_cxx "$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>")
set(_zysu_optimized "$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>,$<CONFIG:RelWithDebInfo>>")
set(_zysu_line_debug "$<OR:$<CONFIG:Release>,$<CONFIG:MinSizeRel>>")
get_filename_component(_zysu_repo_root "${CMAKE_SOURCE_DIR}/../.." ABSOLUTE)
add_compile_options(
    "$<${_zysu_c_or_cxx}:-fstack-protector-strong>"
    # Bionic FORTIFY requires optimization. Level 2 works with the supported NDK
    # without requiring the newer dynamic-object-size implementation of level 3.
    "$<$<AND:${_zysu_c_or_cxx},${_zysu_optimized}>:-U_FORTIFY_SOURCE>"
    "$<$<AND:${_zysu_c_or_cxx},${_zysu_optimized}>:-D_FORTIFY_SOURCE=2>"
    "$<$<AND:${_zysu_c_or_cxx},${_zysu_line_debug}>:-gline-tables-only>"
    # The build ID is content-derived; avoid making debug paths depend on the
    # checkout/build location. Generated version/assets still define build input.
    "$<${_zysu_c_or_cxx}:-ffile-prefix-map=${_zysu_repo_root}=.>"
    "$<${_zysu_c_or_cxx}:-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build>"
    "$<${_zysu_c_or_cxx}:-fdebug-compilation-dir=.>"
)
add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,--build-id=sha1)

# Keep the shipped executable small, but retain symbols AND line tables for
# matching crash addresses to source. Packaging must select the executable
# explicitly; symbols are a separate diagnostic artifact, never installed.
function(zysu_finalize_native target linkage)
    if(NOT CMAKE_STRIP OR NOT EXISTS "${CMAKE_STRIP}" OR
       NOT CMAKE_READELF OR NOT EXISTS "${CMAKE_READELF}")
        message(FATAL_ERROR "Native artifacts require toolchain strip and readelf")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${_repo_root}/scripts/ci/check_native_hardening.py")
    set(_symbols_dir "${CMAKE_CURRENT_BINARY_DIR}/symbols")
    set(_symbols "${_symbols_dir}/${target}.unstripped")
    add_custom_command(TARGET ${target} POST_BUILD
        BYPRODUCTS "${_symbols}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_symbols_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${target}>" "${_symbols}"
        COMMAND "${CMAKE_STRIP}" --strip-all --remove-section=.comment "$<TARGET_FILE:${target}>"
        COMMAND "${Python3_EXECUTABLE}" "${_repo_root}/scripts/ci/check_native_hardening.py"
            --binary "$<TARGET_FILE:${target}>" --symbols "${_symbols}"
            --readelf "${CMAKE_READELF}" --linkage "${linkage}"
            --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
            --configuration "$<CONFIG>"
        VERBATIM
        COMMENT "Preserving ${target} symbols and checking the stripped ELF protections..."
    )
endfunction()
