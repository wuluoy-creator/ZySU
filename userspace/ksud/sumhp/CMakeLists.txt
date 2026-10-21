# Imported from SUMHP cb8345501f3bf8c8f32c844b75751026e55801d1.
set(SUMHP_SOURCES
    src/core/command.cpp src/core/daemon.cpp src/core/log.cpp src/core/log_repeat.cpp
    src/core/config.cpp src/core/runtime.cpp src/core/migration.cpp
    src/core/state_ledger.cpp src/core/state_ledger_model.cpp
    src/core/capability_snapshot.cpp
    src/core/diagnostics.cpp src/core/diagnostics_io.cpp src/core/diagnostics_model.cpp
    src/core/diagnostic_package.cpp src/core/diagnostic_package_model.cpp src/core/diagnostic_package_store.cpp
    src/core/user_hide_store.cpp src/core/user_hide_model.cpp src/core/user_hide.cpp
    src/core/operation_query.cpp src/core/operation_query_input.cpp
    src/core/operation_resource_model.cpp
    src/core/operation_submit.cpp src/core/operation_submit_input.cpp src/core/operation_submit_io.cpp
    src/mount/backend.cpp src/mount/recovery.cpp src/mount/mount_fs.cpp src/mount/sumh.cpp
    src/mount/maps_snapshot.cpp
    src/mount/maps_history.cpp src/mount/maps_history_model.cpp
    src/mount/module_rebuild.cpp src/mount/module_rebuild_model.cpp
    src/mount/maps_preview.cpp
    src/mount/module_preview.cpp src/mount/module_preview_input.cpp
    src/mount/development.cpp src/mount/development_input.cpp src/mount/partition_discovery.cpp
    src/core/profile.cpp src/core/profile_input.cpp src/core/profile_model.cpp src/core/profile_store.cpp
    ../../common/maps_preview_transport.cpp
    ../../common/profile_manager_transport.cpp
    src/mount/query_worker.cpp
    src/mount/sumh_client.cpp src/mount/magic_mount.cpp
    src/mount/overlayfs.cpp src/mount/storage.cpp src/embedded.cpp)
add_library(ksud_sumhp_objs OBJECT ${SUMHP_SOURCES})
target_include_directories(ksud_sumhp_objs BEFORE PRIVATE include src ../src ../../..)
target_compile_options(ksud_sumhp_objs PRIVATE -fexceptions)
zysu_enable_clang_tidy(ksud_sumhp_objs CXX)
