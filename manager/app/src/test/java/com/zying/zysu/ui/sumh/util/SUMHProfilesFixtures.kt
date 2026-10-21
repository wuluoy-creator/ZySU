package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonArray
import com.google.gson.JsonObject

internal object ProfileFixtures {
    val bank = "sha256-v1:" + "a".repeat(64)
    fun profile(hidden: Int = 0): JsonObject = SUMHProfilesParser.json("""{
        "schema_version":1,"kind":"sumh.profile","name":"配置 测试","note":"尾部空格 ","resources":{
        "settings":{"mountsource":"KSU","work_dir":"/dev/sumhp","mirror_dir":"","mirror_img_size_mb":2048,
        "overlay_writable":false,"debug":false,"verbose":false,"fs_type":"auto","enable_kernel_debug":false,
        "enable_stealth":true,"enable_overlay_xattr_hide":false,"enable_mount_hide":false,"mount_hide_mode":"normal",
        "enable_statfs_spoof":false,"enable_kernel_build_spoof":false,"kernel_build_release":"","kernel_build_version":"",
        "kernel_build_apply_stage":"post-fs-data","overlayfs_enabled":true,"magic_mount_enabled":true,"mount_backend":"auto","partitions":[]},
        "modules":[],"user_hide":{"schema_version":1,"rules":[]},"maps":{"enabled":false,"rules":[]}}} """).apply {
        getAsJsonObject("resources").getAsJsonObject("user_hide").add("rules", JsonArray().apply { repeat(hidden) { index -> add(JsonObject().apply {
            addProperty("id", index.toString(16).padStart(32, '0')); addProperty("path", "/synthetic/路径-$index\n ")
            addProperty("name", "条目$index"); addProperty("note", ""); addProperty("group", ""); addProperty("enabled", true)
        }) } })
    }
    fun document(hidden: Int = 0) = SUMHProfilesParser.document(profile(hidden).toString())
    fun base(action: String, id: String? = null) = JsonObject().apply {
        addProperty("schema_version", 1); addProperty("kind", "sumh.profile.result"); addProperty("action", action); addProperty("id", id.orEmpty())
        addProperty("status", "complete"); addProperty("reason", "inactive_profile"); addProperty("saved", false); addProperty("applied", false)
        addProperty("profile_active", false); addProperty("active_configuration_saved", false); addProperty("can_activate", false)
        addProperty("runtime_evaluated", false); addProperty("global_atomic", false)
        if (action != "import-preview") addProperty("bank_revision", bank)
    }
    fun show(action: String = "show", hidden: Int = 0): JsonObject = base(action, "example").apply {
        val document = document(hidden); add("profile", SUMHProfilesParser.json(document.text)); addProperty("profile_revision", SUMHProfilesParser.revision(document))
    }
    fun showRequest(action: String = "show", hidden: Int = 0) = SUMHProfileRequest(action, "example", bank, SUMHProfilesParser.revision(document(hidden)))
    fun listing(): JsonObject = base("list").apply { addProperty("missing", false); add("profiles", JsonArray().apply { add(JsonObject().apply {
        addProperty("id", "example"); addProperty("name", "配置 测试"); addProperty("note", "尾部空格 ")
        addProperty("profile_revision", SUMHProfilesParser.revision(document())); addProperty("profile_active", false)
    }) }) }
    fun mutation(action: String, status: String = "saved"): JsonObject = JsonObject().apply {
        addProperty("schema_version", 1); addProperty("kind", "sumh.profile.mutation"); addProperty("action", if (action in setOf("capture", "import")) "create" else action)
        addProperty("requested_action", action); addProperty("id", "example"); addProperty("status", status); addProperty("reason", "inactive_profile_saved")
        addProperty("detail", ""); addProperty("bank_revision", bank); addProperty("previous_bank_revision", bank)
        if (action == "remove") add("profile_revision", com.google.gson.JsonNull.INSTANCE) else addProperty("profile_revision", SUMHProfilesParser.revision(document()))
        addProperty("saved", status in setOf("saved", "unchanged")); addProperty("applied", false); addProperty("profile_active", false)
        addProperty("active_configuration_saved", false); addProperty("durability_confirmed", status == "saved"); addProperty("readback_confirmed", status in setOf("saved", "unchanged"))
    }
    fun preview(action: String = "import-preview") = base(action).apply {
        addProperty("configuration_diff_only", true); addProperty("reason", "activation_not_implemented")
        addProperty("module_and_capability_applicability", "unknown_not_evaluated"); addProperty("runtime_ownership_and_generation", "not_evaluated")
        addProperty("module_source_contents", "not_captured"); add("selected_resources", JsonArray().apply { add("maps") })
        add("observation", SUMHProfilesParser.json("""{"capture_consistency":"double_read_observation","global_atomic":false,"runtime_verified":false,"stable":true,"source_files_backed_up":false,"settings_defaults_materialized":true,"sources":{}}""").apply {
            val sources = getAsJsonObject("sources")
            listOf("config", "module_modes", "module_rules", "user_hide").forEach { name -> sources.add(name, JsonObject().apply {
                addProperty("status", "valid"); addProperty("errno", 0); addProperty("revision", bank)
            }) }
        })
        add("diff", SUMHProfilesParser.json("""{"resources":{"maps":{"counts":{"added":0,"removed":0,"changed":0,"unchanged":0},"changes":[],"enabled":{"counts":{"added":0,"removed":0,"changed":1,"unchanged":0},"changes":[{"key":"enabled","kind":"change","before":false,"after":true}]}}}}"""))
    }
}
