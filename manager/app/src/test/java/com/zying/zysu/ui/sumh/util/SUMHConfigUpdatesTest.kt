package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONArray
import org.json.JSONObject
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHConfigUpdatesTest {
    @Test
    fun protocolCompatibilityUsesTheNativeExpectedVersion() {
        assertEquals(SUMHUiAdapter.SUMHStatus.NOT_PRESENT, SUMHUiAdapter.kernelStatus(0, 2, false))
        assertEquals(SUMHUiAdapter.SUMHStatus.KERNEL_TOO_OLD, SUMHUiAdapter.kernelStatus(1, 2, false))
        assertEquals(SUMHUiAdapter.SUMHStatus.AVAILABLE, SUMHUiAdapter.kernelStatus(2, 2, true))
        assertEquals(SUMHUiAdapter.SUMHStatus.MODULE_TOO_OLD, SUMHUiAdapter.kernelStatus(3, 2, false))
    }

    private fun configWithLegacyMode() = SUMHUiAdapter.SUMHPConfig(
        original = JSONObject().put("mount_hide_mode", "aggressive"),
        kernelAvailable = true,
    )

    @Test
    fun unchangedConfigurationWithLegacyModeHasNoUpdates() {
        assertEquals(0, SUMHUiAdapter.configUpdates(configWithLegacyMode()).length())
    }

    @Test
    fun mountAndLogEditsDoNotSerializeTheRetiredMountHideMode() {
        val updates = SUMHUiAdapter.configUpdates(configWithLegacyMode().copy(mountBackend = "magic", debug = true))
        assertEquals(setOf("mount_backend", "debug"), updates.keys().asSequence().toSet())
        val plan = planSUMHConfigSave(updates.keys().asSequence().toSet(), true)
        assertFalse(plan.applyRuntime)
        assertEquals(SUMHConfigEffects(rebootRequired = true), plan.effects)
    }

    @Test
    fun mountOptionsRoundTripAndOnlySerializeChangedKeys() {
        val raw = JSONObject("""{"mount_backend":"magic","overlay_writable":true,"mirror_img_size_mb":4096,"partitions":["odm"]}""")
        val config = SUMHUiAdapter.config(raw, emptySet(), true, "")
        assertEquals("magic", config.mountBackend)
        assertTrue(config.overlayWritable)
        assertEquals(4096, config.mirrorImageSizeMb)
        assertEquals(listOf("odm"), config.partitions)
        assertEquals(0, SUMHUiAdapter.configUpdates(config).length())

        val updates = SUMHUiAdapter.configUpdates(config.copy(
            mountBackend = "sumh", overlayWritable = false,
            mirrorImageSizeMb = 8192, partitions = listOf("odm", "my_partition"),
        ))
        assertEquals(setOf("mount_backend", "overlay_writable", "mirror_img_size_mb", "partitions"),
            updates.keys().asSequence().toSet())
        assertEquals(8192, updates.getInt("mirror_img_size_mb"))
        assertEquals("my_partition", updates.getJSONArray("partitions").getString(1))
        assertEquals(SUMHConfigEffects(rebootRequired = true), sumhConfigEffects(updates.keys().asSequence().toSet()))
    }

    @Test
    fun mountOptionValidationRejectsInvalidSizeAndPartitions() {
        val config = SUMHUiAdapter.config(JSONObject(), emptySet(), false, "")
        assertFailsWith<IllegalArgumentException> {
            SUMHUiAdapter.configUpdates(config.copy(mirrorImageSizeMb = 0))
        }
        for (names in listOf(listOf("."), listOf(".."), listOf("a/b"), listOf("odm", "odm"))) {
            assertFailsWith<IllegalArgumentException> {
                SUMHUiAdapter.configUpdates(config.copy(partitions = names))
            }
        }
        assertTrue(validSUMHPartitionName("my_partition"))
        assertFailsWith<IllegalArgumentException> {
            SUMHUiAdapter.configUpdates(config.copy(mountBackend = "sumh"))
        }
        assertEquals("sumh", SUMHUiAdapter.configUpdates(config.copy(kernelAvailable = true,
            mountBackend = "sumh")).getString("mount_backend"))
    }

    @Test
    fun retiredMountHidingFieldsDoNotGenerateMigrationWrites() {
        val raw = JSONObject().put("mount_hide_mode", "aggressive").put("enable_mount_hide", false)
        val config = SUMHUiAdapter.config(raw, emptySet(), true, "")
        val updates = SUMHUiAdapter.configUpdates(config)
        assertEquals(0, updates.length())
    }

    @Test
    fun retiredProtectionFieldsAreNeverReadOrWritten() {
        for (legacyValue in listOf(false, true, "not a boolean", JSONObject.NULL, JSONArray())) {
            val raw = JSONObject().apply { retiredSUMHConfigKeys.forEach { put(it, legacyValue) } }
            val config = SUMHUiAdapter.config(raw, emptySet(), true, "")
            val updates = SUMHUiAdapter.configUpdates(config)
            assertEquals(0, updates.length())
            assertEquals(SUMHUiAdapter.SUMHPConfig(kernelAvailable = true, original = raw), config)
        }
    }

    @Test
    fun savingOtherSettingsNeverSerializesRetiredProtectionFields() {
        val raw = JSONObject().apply { retiredSUMHConfigKeys.forEach { put(it, false) } }
        val config = SUMHUiAdapter.config(raw, emptySet(), true, "").copy(debug = true)
        val updates = SUMHUiAdapter.configUpdates(config)
        assertEquals(setOf("debug"), updates.keys().asSequence().toSet())
        assertTrue(updates.getBoolean("debug"))
    }

    @Test
    fun kernelBuildStageAndFieldsAreRuntimeChanges() {
        val updates = SUMHUiAdapter.configUpdates(configWithLegacyMode().copy(
            kernelBuildRelease = "6.1.75-custom",
            kernelBuildApplyStage = "boot-completed",
        ))
        assertEquals(setOf("kernel_build_release", "kernel_build_apply_stage"), updates.keys().asSequence().toSet())
        assertEquals(SUMHConfigEffects(runtimeRequired = true), sumhConfigEffects(updates.keys().asSequence().toSet()))
    }

    @Test
    fun missingCapabilitiesRemainUnknownWhileEmptyCapabilitiesAreKnown() {
        assertNull(SUMHUiAdapter.parseFeatures(JSONObject().optJSONObject("features")))
        assertNull(SUMHUiAdapter.parseFeatures(JSONObject()))
        assertNull(SUMHUiAdapter.parseFeatures(JSONObject("""{"bitmask":0}""")))
        val empty = assertNotNull(SUMHUiAdapter.parseFeatures(JSONObject("""{"bitmask":0,"names":[]}""")))
        assertEquals(0, empty.bitmask)
        assertEquals(emptyList(), empty.names)
        val supported = assertNotNull(SUMHUiAdapter.parseFeatures(JSONObject("""{"bitmask":64,"names":["mount_hide"]}""")))
        assertEquals(64, supported.bitmask)
        assertEquals(listOf("mount_hide"), supported.names)
    }

    @Test
    fun managedMapsRoundTripPreservesLiteralPathsAndOnlyDiffsChangedKeys() {
        val raw = JSONObject("""{"maps_rules":[{"target_path":"/data/file ","replacement_path":"/system/file "}]}""")
        val config = SUMHUiAdapter.config(raw, setOf("managed_maps"), true, "")
        assertEquals(listOf(SUMHUiAdapter.MapsPathRule("/data/file ", "/system/file ")), config.mapsRules)
        assertEquals(0, SUMHUiAdapter.configUpdates(config).length())
        val unrelated = SUMHUiAdapter.configUpdates(config.copy(debug = true))
        assertEquals(setOf("debug"), unrelated.keys().asSequence().toSet())
        val added = config.copy(mapsRules = config.mapsRules + SUMHUiAdapter.MapsPathRule("/data/second", "/system/second"))
        val updates = SUMHUiAdapter.configUpdates(added)
        assertEquals(setOf("maps_rules"), updates.keys().asSequence().toSet())
        assertEquals(added.mapsRules, SUMHUiAdapter.config(updates, emptySet(), false, "").mapsRules)
        assertEquals(SUMHConfigEffects(runtimeRequired = true), sumhConfigEffects(updates.keys().asSequence().toSet()))
        assertEquals(0, SUMHUiAdapter.configUpdates(config.copy(mapsRules = emptyList())).getJSONArray("maps_rules").length())
    }

    @Test
    fun managedKstatRoundTripAndValidation() {
        val raw = JSONObject("""{"kstat_rules":[{"target_path":"/system/a ","source_path":"/system/b ","mode":"identity"}]}""")
        val config = SUMHUiAdapter.config(raw, setOf("managed_kstat"), true, "")
        assertEquals(listOf(SUMHUiAdapter.KstatPathRule("/system/a ", "/system/b ", "identity")), config.kstatRules)
        assertEquals(0, SUMHUiAdapter.configUpdates(config).length())
        val changed = config.copy(kstatRules = config.kstatRules +
            SUMHUiAdapter.KstatPathRule("/system/c", "/system/d", "full"))
        val updates = SUMHUiAdapter.configUpdates(changed)
        assertEquals(setOf("kstat_rules"), updates.keys().asSequence().toSet())
        assertEquals(changed.kstatRules, SUMHUiAdapter.config(updates, emptySet(), false, "").kstatRules)
        assertEquals(SUMHConfigEffects(runtimeRequired = true), sumhConfigEffects(setOf("kstat_rules")))
        val cleared = SUMHUiAdapter.configUpdates(config.copy(kstatRules = emptyList()))
        assertEquals(setOf("kstat_rules"), cleared.keys().asSequence().toSet())
        assertEquals(0, cleared.getJSONArray("kstat_rules").length())
        for (rules in listOf(
            listOf(changed.kstatRules[0], changed.kstatRules[0].copy(sourcePath = "/system/other")),
            listOf(changed.kstatRules[0].copy(targetPath = "relative")),
            listOf(changed.kstatRules[0].copy(sourcePath = "/bad\npath")),
            listOf(changed.kstatRules[0].copy(mode = "invalid")),
            List(33) { SUMHUiAdapter.KstatPathRule("/target-$it", "/source", "identity") },
        )) assertFailsWith<IllegalArgumentException> {
            SUMHUiAdapter.configUpdates(config.copy(kstatRules = rules))
        }
    }

    @Test
    fun customKstatFieldsRoundTripAndRejectInvalidValues() {
        val fields = mapOf("dev" to "0", "ino" to "18446744073709551615",
            "mtime.sec" to "-1", "mtime.nsec" to "0")
        val raw = JSONObject().put("kstat_rules", kstatRulesJson(listOf(
            SUMHUiAdapter.KstatPathRule("/system/custom", "", "custom", fields))))
        val config = SUMHUiAdapter.config(raw, setOf("managed_kstat"), true, "")
        assertEquals(fields, config.kstatRules.single().customFields)
        assertEquals(0, SUMHUiAdapter.configUpdates(config).length())
        val changed = config.copy(kstatRules = listOf(config.kstatRules.single().copy(
            customFields = fields + ("size" to "0"))))
        val updates = SUMHUiAdapter.configUpdates(changed)
        val encoded = updates.getJSONArray("kstat_rules").getJSONObject(0)
        assertEquals(setOf("target_path", "mode", "fields"), encoded.keys().asSequence().toSet())
        assertEquals("0", encoded.getJSONObject("fields").getString("size"))
        assertEquals("-1", encoded.getJSONObject("fields").getJSONObject("mtime").getString("sec"))
        assertEquals(changed.kstatRules, SUMHUiAdapter.config(updates, emptySet(), false, "").kstatRules)
        for (invalid in listOf(
            emptyMap(), mapOf("ino" to "01"), mapOf("ino" to "-1"),
            mapOf("ino" to "18446744073709551616"), mapOf("dev" to "4294967296"),
            mapOf("mtime.sec" to "0"), mapOf("mtime.sec" to "0", "mtime.nsec" to "1000000000"),
            mapOf("size" to "-1"), mapOf("unknown" to "1"),
        )) assertFailsWith<IllegalArgumentException> {
            SUMHUiAdapter.configUpdates(config.copy(kstatRules = listOf(
                config.kstatRules.single().copy(customFields = invalid))))
        }
    }

    @Test
    fun managedMapsDiffRejectsInvalidDefinitionsBeforeSaving() {
        val config = configWithLegacyMode()
        val valid = SUMHUiAdapter.MapsPathRule("/target", "/replacement")
        for (rules in listOf(
            listOf(valid, valid.copy(replacementPath = "/other")),
            listOf(valid.copy(targetPath = "relative")),
            listOf(valid.copy(replacementPath = "/file\nnext")),
            listOf(valid.copy(replacementPath = "/" + "a".repeat(255))),
            List(257) { valid.copy(targetPath = "/$it") },
        )) assertFailsWith<IllegalArgumentException> { SUMHUiAdapter.configUpdates(config.copy(mapsRules = rules)) }
    }

    @Test
    fun managedMapsBudgetIncludesUtf8AndBothJsonEscapeLayers() {
        val plain = List(256) { SUMHUiAdapter.MapsPathRule("/$it/" + "a".repeat(60), "/replacement") }
        val escaped = List(256) { SUMHUiAdapter.MapsPathRule("/$it/" + "\"".repeat(60), "/replacement") }
        assertTrue(mapsRulesWithinBudget(plain))
        assertFalse(mapsRulesWithinBudget(escaped))
        assertFailsWith<IllegalArgumentException> { SUMHUiAdapter.configUpdates(configWithLegacyMode().copy(mapsRules = escaped)) }
        val fixture = listOf(SUMHUiAdapter.MapsPathRule("/中/\"\\\t", "/plain"))
        val serialized = mapsRulesJson(fixture).toString()
        assertEquals(JSONObject.quote(serialized).toByteArray(Charsets.UTF_8).size + 1, mapsRulesEncodedBytes(fixture))
        assertTrue(mapsRulesEncodedBytes(fixture) > serialized.toByteArray(Charsets.UTF_8).size)
        // Shared C++ fixture pins Android slash escaping, the second layer and daemon comma spaces.
        assertEquals(84, mapsRulesEncodedBytes(fixture))
        assertEquals(4, mapsRulesEncodedBytes(emptyList()))
    }

    @Test
    fun managedMapsBudgetAcceptsExactLimitAndRejectsNextByte() {
        val base = List(14) { SUMHUiAdapter.MapsPathRule("/" + "a".repeat(3890) + it.toString().padStart(4, '0'), "/r") }
        val last = SUMHUiAdapter.MapsPathRule("/", "/replacement")
        val remaining = SUMH_MAX_MAPS_ENCODED_BYTES - mapsRulesEncodedBytes(base + last)
        assertTrue(remaining in 0..4094)
        val atLimit = base + last.copy(targetPath = "/" + "z".repeat(remaining))
        assertEquals(SUMH_MAX_MAPS_ENCODED_BYTES, mapsRulesEncodedBytes(atLimit))
        assertTrue(mapsRulesWithinBudget(atLimit))
        assertEquals(15, SUMHUiAdapter.configUpdates(configWithLegacyMode().copy(mapsRules = atLimit)).getJSONArray("maps_rules").length())
        val over = atLimit.dropLast(1) + atLimit.last().copy(targetPath = atLimit.last().targetPath + "z")
        assertFalse(mapsRulesWithinBudget(over))
        assertFailsWith<IllegalArgumentException> { SUMHUiAdapter.configUpdates(configWithLegacyMode().copy(mapsRules = over)) }
        val edited = assertNotNull(updateSUMHMapsPathRule(atLimit, atLimit.last().targetPath,
            atLimit.last().copy(replacementPath = "/short")))
        assertTrue(mapsRulesWithinBudget(edited))
        assertEquals(atLimit.size, edited.size)
    }

    @Test
    fun editingAPathRuleReplacesOneEntryInOrderWithoutDeletingOthers() {
        val rules = List(256) { SUMHUiAdapter.MapsPathRule("/$it", "/replacement-$it") }
        val replacement = SUMHUiAdapter.MapsPathRule("/edited ", "/new ")
        val edited = assertNotNull(updateSUMHMapsPathRule(rules, "/128", replacement))
        assertEquals(rules.take(128), edited.take(128))
        assertEquals(replacement, edited[128])
        assertEquals(rules.drop(129), edited.drop(129))
        assertEquals(256, edited.size)
        assertEquals(256, SUMHUiAdapter.configUpdates(configWithLegacyMode().copy(mapsRules = edited)).getJSONArray("maps_rules").length())
        assertNull(updateSUMHMapsPathRule(rules, "/removed", replacement))
    }
}
