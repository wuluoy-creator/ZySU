package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONArray
import org.json.JSONObject
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHConfigBackupTest {
    private fun backup(config: JSONObject, paths: List<String> = emptyList()) = JSONObject()
        .put("format", "zysu-sumh")
        .put("version", 1)
        .put("config", config)
        .put("hide_paths", JSONArray(paths))
        .toString()

    @Test
    fun roundTripKeepsStoredConfigAndLiteralPaths() {
        val config = JSONObject().put("debug", true).put("maps_rules", JSONArray().put(
            JSONObject().put("target_path", "/data/my file").put("replacement_path", "/system/file")
        ))
        val parsed = SUMHConfigBackup.parse(SUMHConfigBackup.create(config, listOf("/data/name with spaces")).toJson())
        assertEquals(true, parsed.config.getBoolean("debug"))
        assertEquals("/data/my file", parsed.config.getJSONArray("maps_rules").getJSONObject(0).getString("target_path"))
        assertEquals(listOf("/data/name with spaces"), parsed.hidePaths)
        assertFalse(parsed.toJson().contains("module"))
    }

    @Test
    fun rejectsUnsupportedFieldsAndTypes() {
        for (config in listOf(
            JSONObject().put("unknown", true),
            JSONObject().put("debug", "true"),
            JSONObject().put("mirror_img_size_mb", 1.5),
            JSONObject().put("mount_backend", "invalid"),
            JSONObject().put("partitions", JSONArray().put("system").put("system")),
            JSONObject().put("kernel_build_release", "bad release"),
        )) assertFailsWith<Exception> { SUMHConfigBackup.parse(backup(config)) }
        assertFailsWith<Exception> {
            SUMHConfigBackup.parse(backup(JSONObject().put("debug", true), listOf("/a", "/a")))
        }
        assertFailsWith<Exception> { SUMHConfigBackup.parse(backup(JSONObject().put("debug", true), listOf("relative"))) }
    }

    @Test
    fun backupsDiscardRetiredProtectionFieldsWithoutReadingTheirValues() {
        for (legacyValue in listOf(false, true, "invalid", JSONObject.NULL, JSONArray())) {
            val raw = JSONObject().put("debug", true).apply {
                retiredSUMHConfigKeys.forEach { put(it, legacyValue) }
            }
            val parsed = SUMHConfigBackup.parse(backup(raw))
            assertEquals(setOf("debug"), parsed.config.keys().asSequence().toSet())
            assertTrue(parsed.config.getBoolean("debug"))
            val exported = SUMHConfigBackup.create(raw, emptyList())
            assertEquals(setOf("debug"), exported.config.keys().asSequence().toSet())
            val directlySerialized = JSONObject(SUMHConfigBackup(raw, emptyList()).toJson()).getJSONObject("config")
            assertEquals(setOf("debug"), directlySerialized.keys().asSequence().toSet())
            retiredSUMHConfigKeys.forEach { assertEquals(legacyValue, raw.get(it), it) }
            validateSUMHRestore(parsed, raw)
        }
    }

    @Test
    fun backupWithOnlyRetiredSettingsBecomesAConfigurationNoOp() {
        val raw = JSONObject().apply { retiredSUMHConfigKeys.forEach { put(it, false) } }
        val parsed = SUMHConfigBackup.parse(backup(raw, listOf("/data/hidden")))
        assertEquals(0, parsed.config.length())
        assertEquals(listOf("/data/hidden"), parsed.hidePaths)
        validateSUMHRestore(parsed, raw)
        assertEquals(0, SUMHConfigBackup.parse(parsed.toJson()).config.length())
    }

    @Test
    fun retiredFieldsAreRejectedAsLiveConfigurationUpdates() {
        for (key in retiredSUMHConfigKeys) {
            assertFailsWith<IllegalArgumentException> { validateConfig(JSONObject().put(key, true)) }
        }
    }

    @Test
    fun rejectsInvalidMapsAndOversizedBackup() {
        val invalid = JSONArray().put(JSONObject().put("target_path", "/a").put("replacement_path", "relative"))
        assertFailsWith<Exception> { SUMHConfigBackup.parse(backup(JSONObject().put("maps_rules", invalid))) }
        assertFailsWith<Exception> { SUMHConfigBackup.parse(" ".repeat(SUMHConfigBackup.MAX_BYTES + 1)) }
    }

    @Test
    fun kstatRulesSurviveBackupAndRejectMalformedDefinitions() {
        val valid = JSONArray().put(JSONObject()
            .put("target_path", "/system/target")
            .put("source_path", "/system/source")
            .put("mode", "full"))
        val config = JSONObject().put("kstat_rules", valid)
        assertEquals("full", SUMHConfigBackup.parse(backup(config)).config
            .getJSONArray("kstat_rules").getJSONObject(0).getString("mode"))
        val invalid = JSONArray().put(JSONObject()
            .put("target_path", "/system/target")
            .put("source_path", "relative")
            .put("mode", "full"))
        assertFailsWith<Exception> { SUMHConfigBackup.parse(backup(JSONObject().put("kstat_rules", invalid))) }
        val custom = JSONArray().put(JSONObject()
            .put("target_path", "/system/custom")
            .put("mode", "custom")
            .put("fields", JSONObject().put("ino", "0")
                .put("ctime", JSONObject().put("sec", "-1").put("nsec", "0"))))
        val restored = SUMHConfigBackup.parse(backup(JSONObject().put("kstat_rules", custom)))
        assertEquals("0", restored.config.getJSONArray("kstat_rules").getJSONObject(0)
            .getJSONObject("fields").getString("ino"))
        val malformed = JSONArray().put(JSONObject(custom.getJSONObject(0).toString())
            .put("source_path", "/unexpected"))
        assertFailsWith<Exception> {
            SUMHConfigBackup.parse(backup(JSONObject().put("kstat_rules", malformed)))
        }
    }

    @Test
    fun validatesMergedKernelBuildAndAllowsExactByteLimit() {
        val release = "r".repeat(64)
        val parsedBackup = SUMHConfigBackup.parse(backup(JSONObject()
            .put("enable_kernel_build_spoof", true)
            .put("kernel_build_release", release)))
        validateSUMHRestore(parsedBackup, JSONObject().put("kernel_build_version", ""))
        assertFailsWith<IllegalArgumentException> {
            validateSUMHRestore(
                SUMHConfigBackup.parse(backup(JSONObject().put("enable_kernel_build_spoof", true))),
                JSONObject().put("kernel_build_release", "").put("kernel_build_version", ""),
            )
        }
        assertFailsWith<Exception> {
            SUMHConfigBackup.parse(backup(JSONObject().put("kernel_build_release", "r".repeat(65))))
        }
    }

    @Test
    fun structuralComparisonIgnoresObjectOrderButChecksArrayOrder() {
        assertEquals(true, sumhJsonEqual(
            JSONObject("""{"debug":true,"maps_rules":[{"target_path":"/a","replacement_path":"/b"}]}"""),
            JSONObject("""{"maps_rules":[{"replacement_path":"/b","target_path":"/a"}],"debug":true}"""),
        ))
        assertEquals(false, sumhJsonEqual(JSONArray(listOf("/a", "/b")), JSONArray(listOf("/b", "/a"))))
    }
}
