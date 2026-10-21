package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHCapabilitiesTest {
    private val fixtures = SUMHCapabilitiesFixtures
    private fun JSONObject.section(name: String) = getJSONObject("sections").getJSONObject(name)
    private fun JSONObject.payload(name: String) = section(name).getJSONObject("value")
    private fun parsed(raw: JSONObject) = SUMHCapabilitiesParser.parse(raw.toString())
    private fun invalid(raw: JSONObject) = assertNull(SUMHCapabilitiesParser.reply(SUMHQueryReply(raw.toString())).snapshot)
    private fun live(raw: JSONObject, index: Int) = raw.payload("runtime").getJSONObject("runtime").getJSONArray("features").getJSONObject(index)

    @Test fun productionNativeSerializerFixtureParsesWithoutContractRewriting() {
        // Exact output of the host harness executing capability_snapshot.cpp; no Kotlin reserialization.
        val bytes = requireNotNull(javaClass.getResourceAsStream("/sumh/capability-snapshot-v1.json")).use { it.readBytes() }
        val sample = SUMHCapabilitiesParser.parse(bytes.toString(Charsets.UTF_8))
        assertEquals("00000000-0000-0000-0000-000000000001", sample.bootId)
        assertEquals("normal", sample.saved.value!!.mountHideMode)
        assertEquals("9", sample.runtime.value!!.sample.instanceId)
        assertEquals("android14-6.1", sample.build.value!!.originalKmiLabel)
        assertNull(sample.owner.value)
        assertEquals("sampled_flags_differ", sample.features().single { it.id == "stealth" }.reason)
    }

    @Test fun metadataRequiresEveryGranularReadOnlyContractField() {
        assertEquals(SUMHSchemaSupport.Supported, SUMHCapabilitiesParser.capabilities(SUMHQueryReply(fixtures.metadata().toString())))
        assertEquals(SUMHSchemaSupport.NotAdvertised, SUMHCapabilitiesParser.capabilities(SUMHQueryReply("{\"protocol_version\":999}")))
        for (key in fixtures.metadata().keys()) {
            val meta = fixtures.metadata().also { it.remove(key) }
            assertEquals(SUMHSchemaSupport.Invalid, SUMHCapabilitiesParser.capabilities(SUMHQueryReply(meta.toString())))
        }
        assertEquals(SUMHSchemaSupport.Unavailable, SUMHCapabilitiesParser.capabilities(SUMHQueryReply("{}", "offline")))
        assertEquals(SUMHSchemaSupport.Future, SUMHCapabilitiesParser.capabilities(SUMHQueryReply(fixtures.metadata().put("capability_snapshot_schema", 2).toString())))
    }

    @Test fun nativeFailureFixturesKeepStructuredReasonsWithoutInventingValues() {
        for ((name, reason) in listOf("boot-missing" to "boot_identity_unavailable",
            "failure" to "capability_query_failed")) {
            val bytes = requireNotNull(javaClass.getResourceAsStream("/sumh/capability-snapshot-v1-$name.json"))
                .use { it.readBytes() }
            val sample = SUMHCapabilitiesParser.parse(bytes.toString(Charsets.UTF_8))
            assertNull(sample.bootId)
            sample.sections.forEach { (_, section) ->
                assertEquals(SUMHCapabilityStatus.Unknown, section.status)
                assertEquals(reason, section.reason)
                assertNull(section.value)
            }
        }
    }

    @Test fun observedReleaseSpacesArePreservedWhileSavedOverridesRemainStrict() {
        val raw = fixtures.document()
        raw.payload("build").put("release", "6.1 spaced release").put("original_release", "original release ")
        val sample = parsed(raw)
        assertEquals("6.1 spaced release", sample.build.value!!.release)
        assertEquals("original release ", sample.build.value!!.originalRelease)
        assertNotNull(sample.saved.value); assertNotNull(sample.runtime.value)
        raw.payload("saved").put("kernel_build_release", "6.1 spaced release")
        invalid(raw)
    }

    @Test fun observedBuildFieldsRetainUtf8ByteAndControlBounds() {
        for (field in listOf("release", "version", "original_release", "original_version")) {
            val raw = fixtures.document()
            raw.payload("build").put(field, "汉".repeat(21) + "x")
            assertNotNull(parsed(raw).build.value)
            for (bad in listOf("", "汉".repeat(21) + "xx", "bad\nvalue", "bad\u007fvalue", "\u0000", "\ud800")) {
                raw.payload("build").put(field, bad)
                invalid(raw)
            }
        }
    }

    @Test fun metadataRejectsCoercionAndWrongBudgets() {
        for ((key, bad) in listOf("capability_snapshot_schema" to "1", "capability_snapshot_read_only" to "true",
            "capability_snapshot_read_only" to false, "capability_snapshot_response_bytes" to 65537))
            assertEquals(SUMHSchemaSupport.Invalid, SUMHCapabilitiesParser.capabilities(SUMHQueryReply(fixtures.metadata().put(key, bad).toString())))
    }

    @Test fun allValuesAreTypedAndIndependentWithNoInventedLiveOwner() {
        val sample = fixtures.parsed()
        assertEquals("normal", sample.saved.value!!.mountHideMode)
        assertEquals("22", sample.runtime.value!!.sample.instanceId)
        assertEquals("4", sample.build.value!!.generation)
        assertEquals("3", sample.runtime.value!!.sample.generation)
        assertNull(sample.owner.value)
        assertEquals(SUMHCapabilityStatus.Unknown, sample.owner.status)
        assertEquals(SUMHCapabilityBackendStatus.Unknown, sample.backends.value!!.items.single { it.id == "overlay" }.status)
        assertEquals(10, sample.features().size)
    }

    @Test fun savedAndKernelFlagsNeverOverwriteOneAnother() {
        val raw = fixtures.document()
        raw.payload("saved").put("enable_stealth", true)
        val row = parsed(raw).features().single { it.id == "stealth" }
        assertEquals(true, row.saved); assertEquals(false, row.configured); assertEquals(false, row.active)
        assertEquals(SUMHComparison.Drift, row.comparison); assertEquals("sampled_flags_differ", row.reason)
    }

    @Test fun inactiveConfiguredIsNotUnsupportedOrDisabled() {
        val raw = fixtures.document()
        raw.payload("saved").put("enable_stealth", true)
        live(raw, 1).put("configured", true).put("degraded", true)
        val row = parsed(raw).features().single { it.id == "stealth" }
        assertEquals(true, row.supported); assertEquals(true, row.configured); assertEquals(false, row.active)
        assertEquals(true, row.degraded); assertEquals(SUMHComparison.Matches, row.comparison)
        assertEquals("configured_but_inactive", row.reason)
    }

    @Test fun unsupportedControlDiffersFromOff() {
        val raw = fixtures.document(); live(raw, 1).put("supported", false)
        val row = parsed(raw).features().single { it.id == "stealth" }
        assertEquals(false, row.supported); assertEquals(SUMHComparison.Unsupported, row.comparison)
        assertEquals(SUMHComparison.Matches, parsed(raw).features().single { it.id == "mount_hide" }.comparison)
    }

    @Test fun resourcesDoNotInventRuntimeReadinessOrUserObservations() {
        for (row in fixtures.parsed().features().filter { it.id in listOf("user_hide", "maps_rules", "modules", "overlay_xattr_hide") }) {
            assertNull(row.supported); assertNull(row.configured); assertNull(row.active)
            assertEquals(SUMHComparison.Unknown, row.comparison)
        }
        assertEquals(SUMHComparison.Unknown, fixtures.parsed().features().single { it.id == "kernel_build_spoof" }.comparison)
    }

    @Test fun kmiInferenceUsesOriginalReleaseOnly() {
        val raw = fixtures.document(); raw.payload("build").put("release", "9.9.9-android99-fake")
        assertEquals("android15-6.6", parsed(raw).build.value!!.originalKmiLabel)
        raw.payload("build").put("original_release", "custom")
        assertNull(parsed(raw).build.value!!.originalKmiLabel)
    }

    @Test fun nullMissingAndWrongBooleansNeverBecomeOff() {
        for (value in listOf(JSONObject.NULL, "false", 0)) {
            val raw = fixtures.document(); raw.payload("saved").put("enable_stealth", value); invalid(raw)
        }
        val raw = fixtures.document(); raw.payload("saved").remove("enable_stealth"); invalid(raw)
    }

    @Test fun savedMountModeUsesActualConfigurationVocabulary() {
        val raw = fixtures.document(); raw.payload("saved").put("mount_hide_mode", "aggressive")
        assertEquals("aggressive", parsed(raw).saved.value!!.mountHideMode)
        for (bad in listOf<Any>(1, "strict", "unknown")) { raw.payload("saved").put("mount_hide_mode", bad); invalid(raw) }
    }

    @Test fun unknownRuntimeRetainsOldControlsAndLatestCapabilityAttemptSeparately() {
        val old = fixtures.parsed()
        val raw = fixtures.document()
        raw.section("runtime").put("status", "error").put("reason", "runtime_query_failed").put("errno", 5)
        raw.payload("runtime").put("status", "error").put("reason", "runtime_query_failed").put("errno", 5).put("runtime", JSONObject.NULL)
        val combined = parsed(raw).retaining(old)
        assertTrue(combined.runtime.retained)
        assertEquals("3", combined.runtime.value!!.sample.generation)
        assertEquals(SUMHCapabilityStatus.Error, combined.runtime.attempt!!.status)
        assertEquals("786432", combined.runtime.attempt!!.value!!.capabilityBits)
        assertNull(combined.runtime.attempt!!.value!!.sample.generation)
        assertEquals(SUMHComparison.Unknown, combined.features().first().comparison)
    }

    @Test fun partialRetentionPreservesBootIntervalAndFailureOnlyForFailedSection() {
        val old = fixtures.parsed()
        val raw = fixtures.document()
        raw.put("boot_id", "boot-b"); raw.payload("runtime").put("boot_id", "boot-b")
        raw.section("saved").put("status", "error").put("reason", "saved_config_unreadable").put("errno", 13).put("value", JSONObject.NULL)
        val combined = parsed(raw).retaining(old)
        assertEquals("boot-b", combined.bootId); assertEquals("boot-a", combined.saved.bootId)
        assertEquals("10", combined.saved.startedMs); assertEquals("20", combined.saved.finishedMs)
        assertTrue(combined.saved.retained); assertFalse(combined.runtime.retained)
        assertEquals("boot-b", combined.saved.attempt!!.bootId)
        assertEquals("saved_config_unreadable (errno=13)", combined.saved.error)
        assertEquals(SUMHComparison.Unknown, combined.features().first().comparison)
    }

    @Test fun wholeFailureKeepsOldProvenanceButFreshSuccessClearsRetention() {
        val old = SUMHCapabilitiesResult(fixtures.parsed())
        val failed = SUMHCapabilitiesResult(error = "timeout").retaining(old)
        assertEquals("boot-a", failed.snapshot!!.bootId); assertTrue(failed.snapshot!!.hasRetained)
        assertEquals("timeout", failed.snapshot!!.saved.error)
        assertFalse(old.retaining(failed).snapshot!!.hasRetained)
    }

    @Test fun explicitUnsupportedClearsPriorKnownBuildInsteadOfRetainingIt() {
        val raw = fixtures.document(); raw.section("build").put("status", "unsupported").put("reason", "kernel_build_not_advertised").put("value", JSONObject.NULL)
        val updated = parsed(raw).retaining(fixtures.parsed())
        assertNull(updated.build.value); assertFalse(updated.build.retained)
    }

    @Test fun repeatedRetentionDoesNotGrowAttemptChains() {
        val raw = fixtures.document(); raw.section("saved").put("status", "unknown").put("reason", "saved_config_missing").put("value", JSONObject.NULL)
        val first = parsed(raw).retaining(fixtures.parsed())
        val second = parsed(raw).retaining(first)
        assertNotNull(second.saved.attempt); assertNull(second.saved.attempt!!.attempt)
    }

    @Test fun earlyGuardAndBootErrorsRemainStructuredWithoutKnownValues() {
        for (reason in listOf("boot_identity_unavailable", "boot_changed_during_sample", "capability_query_guard_unavailable",
            "reply_memory_unavailable", "capability_snapshot_response_capacity", "capability_query_failed")) {
            val raw = fixtures.document().put("boot_id", JSONObject.NULL)
            for (name in listOf("saved", "runtime", "build", "backends", "owner"))
                raw.section(name).put("status", "unknown").put("reason", reason).put("value", JSONObject.NULL)
            val sample = parsed(raw)
            assertNull(sample.bootId); assertEquals(reason, sample.runtime.reason)
            raw.section("saved").put("value", fixtures.document().payload("saved")); invalid(raw)
        }
    }

    @Test fun runtimeEncodingFailureDoesNotDiscardOtherSections() {
        val raw = fixtures.document(); raw.section("runtime").put("status", "error").put("reason", "runtime_encoding_failed").put("errno", 5).put("value", JSONObject.NULL)
        assertNull(parsed(raw).runtime.value); assertNotNull(parsed(raw).saved.value)
        raw.section("runtime").put("status", "known"); invalid(raw)
    }

    @Test fun nestedBootStatusAndTimeMustAgreeWithOwnSection() {
        for (change in listOf<(JSONObject) -> Unit>(
            { it.payload("runtime").put("boot_id", "another") },
            { it.payload("runtime").put("sample_started_monotonic_ms", "9") },
            { it.section("runtime").put("status", "unsupported") },
            { it.section("runtime").put("errno", 5) },
            { it.section("saved").put("sample_finished_monotonic_ms", "31") },
            { it.section("saved").put("sample_started_monotonic_ms", "21") },
        )) { val raw = fixtures.document(); change(raw); invalid(raw) }
    }

    @Test fun exactSourcesAndBackendCompletenessAreEnforced() {
        val raw = fixtures.document(); raw.section("build").put("source", "kernel_build_ioctl"); invalid(raw)
        val duplicate = fixtures.document(); val items = duplicate.payload("backends").getJSONArray("items")
        items.put(1, items.getJSONObject(0)); invalid(duplicate)
    }

    @Test fun buildTokenAndUtf8LimitsFollowDeclaredContract() {
        val raw = fixtures.document(); raw.payload("build").put("instance_id", JSONObject.NULL); invalid(raw)
        val independent = fixtures.document(); independent.payload("build").put("consistency", "independently_sampled")
            .put("instance_id", JSONObject.NULL).put("generation", JSONObject.NULL)
        assertNotNull(parsed(independent).build.value)
        val long = fixtures.document(); long.payload("saved").put("kernel_build_version", "汉".repeat(22)); invalid(long)
        val invalidSurrogate = fixtures.document(); invalidSurrogate.payload("saved").put("kernel_build_version", "\ud800"); invalid(invalidSurrogate)
    }

    @Test fun duplicateTrailingFutureAndOverbudgetPayloadsFailClosed() {
        val good = fixtures.document().toString()
        for (bad in listOf(good + " {}", good.replaceFirst("{", "{\"schema_version\":1,"),
            fixtures.document().put("schema_version", 2).toString(), " ".repeat(SUMH_CAPABILITIES_BYTES) + good))
            assertNull(SUMHCapabilitiesParser.reply(SUMHQueryReply(bad)).snapshot)
    }

    @Test fun validNonzeroEnvelopePreservesPartialFactsAndError() {
        val envelope = JSONObject().put("ok", false).put("exit_code", -110).put("errno", 110).put("stderr", "timeout after response")
            .put("stdout", fixtures.document().toString()).toString().toByteArray()
        val result = SUMHCapabilitiesParser.reply(SUMHCapabilitiesParser.envelope(envelope))
        assertNotNull(result.snapshot); assertEquals("timeout after response", result.error)
    }

    @Test fun invalidUtf8EnvelopeAndOversizeAreRejectedBeforeParsing() {
        assertFails { SUMHCapabilitiesParser.envelope(byteArrayOf(0xC3.toByte(), 0x28)) }
        assertFails { SUMHCapabilitiesParser.envelope(ByteArray(512 * 1024 + 1)) }
    }
}
