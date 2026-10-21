# Lock the parent declarations before any embedded subproject can declare its
# own mutable fallbacks. FetchContent keeps the first declaration of each name.
# Python is already required for ksud assets; it also supports CMake 3.14 here,
# which has no string(JSON) API.
include_guard(GLOBAL)
include(FetchContent)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_ZYSU_DEPENDENCY_HELPER "${CMAKE_CURRENT_LIST_DIR}/native_dependencies.py")
set(_ZYSU_DEPENDENCY_LOCK "${CMAKE_CURRENT_LIST_DIR}/native-dependencies.lock.json")
set(_zysu_declarations "${CMAKE_BINARY_DIR}/native-dependency-declarations.cmake")
# A failed reconfigure must not leave an earlier dependency resolution looking
# like the result of the new configure attempt.
file(REMOVE "${CMAKE_BINARY_DIR}/native-dependencies.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_ZYSU_DEPENDENCY_HELPER}" "${_ZYSU_DEPENDENCY_LOCK}")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_ZYSU_DEPENDENCY_HELPER}" declarations
            --lock "${_ZYSU_DEPENDENCY_LOCK}" --output "${_zysu_declarations}"
    RESULT_VARIABLE _zysu_dependency_result
    ERROR_VARIABLE _zysu_dependency_error
)
if(NOT _zysu_dependency_result EQUAL 0)
    message(FATAL_ERROR "Invalid native dependency lock: ${_zysu_dependency_error}")
endif()
include("${_zysu_declarations}")

# Call after all FetchContent/subprojects are configured. The source directories
# are observations, not proof of an archive hash or a clean immutable checkout.
function(zysu_record_native_dependencies lz4_kind lz4_dir)
    set(_args metadata --lock "${_ZYSU_DEPENDENCY_LOCK}"
        --output "${CMAKE_BINARY_DIR}/native-dependencies.json"
        --fully-disconnected "${FETCHCONTENT_FULLY_DISCONNECTED}")
    foreach(_name IN ITEMS mbedtls miniz lz4 magisk_lz4 magisk_xz)
        FetchContent_GetProperties(${_name})
        if(${_name}_POPULATED)
            string(TOUPPER "${_name}" _upper)
            set(_override OFF)
            if(FETCHCONTENT_SOURCE_DIR_${_upper})
                set(_override ON)
            endif()
            set(_updates "${FETCHCONTENT_UPDATES_DISCONNECTED}")
            if(FETCHCONTENT_UPDATES_DISCONNECTED_${_upper})
                set(_updates ON)
            endif()
            list(APPEND _args --source "${_name}=${${_name}_SOURCE_DIR}"
                --override "${_name}=${_override}"
                --updates-disconnected "${_name}=${_updates}")
        endif()
    endforeach()
    if(lz4_kind STREQUAL "vendored")
        list(APPEND _args --source "vendored_lz4=${lz4_dir}")
    elseif(NOT lz4_kind STREQUAL "fetchcontent")
        message(FATAL_ERROR "Unknown LZ4 dependency resolution")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${_ZYSU_DEPENDENCY_HELPER}" ${_args}
        RESULT_VARIABLE _result ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "Unable to record native dependencies: ${_error}")
    endif()
endfunction()
