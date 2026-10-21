# CMake toolchains may set ordinary variables instead of cache entries. Record
# the actual configured values; CMakeCache.txt alone is not a tool identity.
include_guard(GLOBAL)
file(REMOVE "${CMAKE_BINARY_DIR}/native-toolchain.json")

function(_zysu_toolchain_json_string output value)
    # CMake strings cannot contain NUL. Escape JSON whitespace controls and
    # reject every other C0 control before writing a consumable metadata file.
    foreach(_code RANGE 1 31)
        if(NOT _code EQUAL 9 AND NOT _code EQUAL 10 AND NOT _code EQUAL 13)
            string(ASCII ${_code} _control)
            string(FIND "${value}" "${_control}" _index)
            if(NOT _index EQUAL -1)
                message(FATAL_ERROR "Unsupported control character in native toolchain metadata")
            endif()
        endif()
    endforeach()
    string(REPLACE "\\" "\\\\" _escaped "${value}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    string(REPLACE "\t" "\\t" _escaped "${_escaped}")
    string(REPLACE "\r" "\\r" _escaped "${_escaped}")
    string(REPLACE "\n" "\\n" _escaped "${_escaped}")
    set(${output} "\"${_escaped}\"" PARENT_SCOPE)
endfunction()

function(zysu_record_native_toolchain target)
    if(NOT target MATCHES "^(ksud|ksuinit)$")
        message(FATAL_ERROR "Unsupported native provenance target")
    endif()
    # NativeHardening's scoped find_package may not expose Python to its caller.
    # Resolve it in this scope too, just as the build's asset/check commands do.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(NOT CMAKE_CXX_COMPILER)
        message(FATAL_ERROR "Native toolchain metadata requires the configured C++ compiler")
    endif()
    set(_keys
        CMAKE_BUILD_TYPE CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_ANDROID_ARCH_ABI
        CMAKE_ANDROID_NDK CMAKE_GENERATOR CMAKE_C_FLAGS CMAKE_CXX_FLAGS
        CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE CMAKE_EXE_LINKER_FLAGS
        CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_TOOLCHAIN_FILE KSUD_DISABLE_LTO
        KSUD_UPX KSUINIT_UPX ZYSU_ENABLE_CLANG_TIDY CMAKE_SYSROOT
        CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_LINKER CMAKE_AR CMAKE_RANLIB
        CMAKE_STRIP CMAKE_READELF CMAKE_MAKE_PROGRAM CMAKE_COMMAND
        CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER _Python3_EXECUTABLE
        Python3_EXECUTABLE)
    set(_json "{\n  \"schema_version\": 1,\n  \"target\": \"${target}\",\n  \"values\": {")
    set(_separator "")
    foreach(_key IN LISTS _keys)
        if(DEFINED ${_key} AND NOT "${${_key}}" STREQUAL "")
            _zysu_toolchain_json_string(_value "${${_key}}")
            string(APPEND _json "${_separator}\n    \"${_key}\": ${_value}")
            set(_separator ",")
        endif()
    endforeach()
    string(APPEND _json "\n  }\n}\n")
    file(WRITE "${CMAKE_BINARY_DIR}/native-toolchain.json.tmp" "${_json}")
    file(RENAME "${CMAKE_BINARY_DIR}/native-toolchain.json.tmp"
                "${CMAKE_BINARY_DIR}/native-toolchain.json")
endfunction()
