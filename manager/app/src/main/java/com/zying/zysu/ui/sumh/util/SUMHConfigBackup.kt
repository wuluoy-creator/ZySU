package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject

internal data class SUMHConfigBackup(val config: JSONObject, val hidePaths: List<String>) {
    fun toJson(): String = JSONObject()
        .put("format", FORMAT)
        .put("version", VERSION)
        .put("config", config)
        .put("hide_paths", JSONArray(hidePaths))
        .toString(2)

    companion object {
        private const val FORMAT = "zysu-sumh"
        private const val VERSION = 1
        const val MAX_BYTES = 256 * 1024

        fun parse(text: String): SUMHConfigBackup {
            require(text.toByteArray(Charsets.UTF_8).size <= MAX_BYTES) { "Backup exceeds 256 KiB" }
            val root = JSONObject(text)
            require(root.keys().asSequence().toSet() == setOf("format", "version", "config", "hide_paths")) {
                "Unexpected backup fields"
            }
            require(root.get("format") == FORMAT && root.get("version") == VERSION) { "Unsupported SUMH backup version" }
            val config = root.getJSONObject("config")
            validateConfig(config)
            val entries = root.getJSONArray("hide_paths")
            val paths = (0 until entries.length()).map { index ->
                val path = entries.get(index) as? String ?: error("Hidden path ${index + 1} must be text")
                require(validateSUMHHidePath(path) == null) { "Invalid hidden path ${index + 1}" }
                path
            }
            require(paths.distinct().size == paths.size) { "Duplicate hidden paths" }
            return SUMHConfigBackup(JSONObject(config.toString()), paths)
        }

        fun create(config: JSONObject, hidePaths: List<String>): SUMHConfigBackup =
            parse(SUMHConfigBackup(config, hidePaths).toJson())
    }
}

private val booleanKeys = setOf(
    "overlay_writable", "debug", "verbose", "enable_kernel_debug", "enable_stealth",
    "enable_overlay_xattr_hide", "enable_mount_hide", "enable_maps_spoof",
    "enable_statfs_spoof", "enable_kernel_build_spoof", "overlayfs_enabled", "magic_mount_enabled",
)
private val stringKeys = setOf(
    "mountsource", "work_dir", "mirror_dir", "fs_type", "mount_hide_mode",
    "kernel_build_release", "kernel_build_version", "kernel_build_apply_stage", "mount_backend",
)
private val arrayKeys = setOf("maps_rules", "kstat_rules", "partitions")
private val configKeys = booleanKeys + stringKeys + arrayKeys + "mirror_img_size_mb"

internal fun validateConfig(config: JSONObject) {
    val keys = config.keys().asSequence().toSet()
    require(keys.isNotEmpty() && keys.all { it in configKeys }) { "Unknown or empty SUMH configuration" }
    for (key in keys) {
        val value = config.get(key)
        require(when (key) {
            in booleanKeys -> value is Boolean
            in stringKeys -> value is String && '\u0000' !in value
            "mirror_img_size_mb" -> value is Number && value.toDouble().isFinite() &&
                value.toDouble() % 1.0 == 0.0 && value.toDouble() in 1.0..Int.MAX_VALUE.toDouble()
            in arrayKeys -> value is JSONArray
            else -> false
        }) { "Invalid configuration field: $key" }
    }
    fun string(key: String) = if (config.has(key)) config.getString(key) else null
    require(string("work_dir")?.startsWith('/') != false) { "Invalid work directory" }
    require(string("mirror_dir")?.let { it.isEmpty() || it.startsWith('/') } != false) { "Invalid mirror directory" }
    require(string("fs_type")?.let { it in setOf("", "auto", "tmpfs", "ext4", "erofs") } != false) { "Invalid filesystem type" }
    require(string("mount_backend")?.let { it.isEmpty() || it in SUMHManager.mountModes } != false) { "Invalid mount backend" }
    require(string("mount_hide_mode")?.let { it in setOf("", "normal", "aggressive") } != false) { "Invalid mount hide mode" }
    require(string("kernel_build_apply_stage")?.let { it in setOf("post-fs-data", "boot-completed") } != false) { "Invalid kernel build stage" }
    for (key in listOf("kernel_build_release", "kernel_build_version")) {
        string(key)?.let { value ->
            require(value.toByteArray(Charsets.UTF_8).size <= 64 && value.none { it.code < 0x20 || it.code == 0x7f }) {
                "Invalid $key"
            }
            if (key == "kernel_build_release") require(' ' !in value) { "Invalid kernel release" }
        }
    }
    config.optJSONArray("partitions")?.let { entries ->
        val partitions = (0 until entries.length()).map { entries.get(it) as? String ?: error("Invalid partition") }
        require(partitions.all(::validSUMHPartitionName) && partitions.distinct().size == partitions.size) {
            "Invalid or duplicate partitions"
        }
    }
    config.optJSONArray("maps_rules")?.let { entries ->
        require(entries.length() <= SUMH_MAX_MAPS_PATH_RULES) { "Too many maps rules" }
        val rules = (0 until entries.length()).map { index ->
            val item = entries.get(index) as? JSONObject ?: error("Invalid maps rule ${index + 1}")
            require(item.keys().asSequence().toSet() == setOf("target_path", "replacement_path")) {
                "Invalid maps rule ${index + 1}"
            }
            val target = item.get("target_path") as? String ?: error("Invalid maps target ${index + 1}")
            val replacement = item.get("replacement_path") as? String ?: error("Invalid maps replacement ${index + 1}")
            require(validateSUMHManagedMapsPath(target, 4095) == null &&
                validateSUMHManagedMapsPath(replacement) == null) { "Invalid maps path ${index + 1}" }
            SUMHUiAdapter.MapsPathRule(target, replacement)
        }
        require(rules.map { it.targetPath }.distinct().size == rules.size && mapsRulesWithinBudget(rules)) {
            "Duplicate or oversized maps rules"
        }
    }
    config.optJSONArray("kstat_rules")?.let { entries ->
        val rules = (0 until entries.length()).map { index ->
            val item = entries.get(index) as? JSONObject ?: error("Invalid kstat rule ${index + 1}")
            parseKstatRule(item)
        }
        require(validSUMHKstatRules(rules)) { "Invalid kstat rules" }
    }
}

internal fun validateSUMHRestore(backup: SUMHConfigBackup, current: JSONObject) {
    val merged = JSONObject(current.toString())
    backup.config.keys().forEach { key -> merged.put(key, backup.config.get(key)) }
    validateConfig(merged)
    require(!merged.optBoolean("enable_kernel_build_spoof") ||
        merged.optString("kernel_build_release").isNotEmpty() ||
        merged.optString("kernel_build_version").isNotEmpty()) {
        "Kernel build spoof requires a custom release or version"
    }
}

internal fun sumhJsonEqual(left: Any?, right: Any?): Boolean = when {
    left is JSONObject && right is JSONObject -> {
        val keys = left.keys().asSequence().toSet()
        keys == right.keys().asSequence().toSet() && keys.all {
            sumhJsonEqual(left.get(it), right.get(it))
        }
    }
    left is JSONArray && right is JSONArray -> left.length() == right.length() &&
        (0 until left.length()).all { sumhJsonEqual(left.get(it), right.get(it)) }
    left is Number && right is Number -> left.toDouble() == right.toDouble()
    else -> left == right
}
