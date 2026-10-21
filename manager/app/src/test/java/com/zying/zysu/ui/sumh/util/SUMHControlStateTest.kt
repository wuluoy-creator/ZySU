package com.zying.zysu.ui.sumh.util

import android.app.Application
import com.zying.zysu.ui.sumh.SUMHApplyAdvice
import com.zying.zysu.ui.sumh.applyAdvice
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHControlStateTest {
    private val fixtures = SUMHControlStateFixtures
    private fun parse(value: JSONObject) = SUMHControlStateParser.state(SUMHQueryReply(value.toString()))
    private fun runtime(value: JSONObject) = SUMHControlStateParser.runtime(SUMHQueryReply(value.toString()))

    @Test fun independentSchemasNeverFollowProtocolVersion() {
        val meta = JSONObject().put("protocol_version", 1).put("runtime_status_schema", 1)
        val caps = SUMHControlStateParser.capabilities(meta)
        assertEquals(SUMHSchemaSupport.NotAdvertised, caps.state)
        assertEquals(SUMHSchemaSupport.Supported, caps.runtime)
        for (invalid in listOf(JSONObject.NULL, "1", true, 0, -1, 1.5)) {
            meta.put("state_ledger_schema", invalid)
            assertEquals(SUMHSchemaSupport.Invalid, SUMHControlStateParser.capabilities(meta).state)
        }
        meta.put("state_ledger_schema", 2)
        assertEquals(SUMHSchemaSupport.Future, SUMHControlStateParser.capabilities(meta).state)
    }
    @Test fun validRuntimeRetainsUnsigned64Exactly() {
        val raw = fixtures.runtime()
        raw.getJSONObject("runtime").put("instance_id", "18446744073709551615")
        assertEquals("18446744073709551615", runtime(raw).value?.instanceId)
        assertEquals(false, runtime(raw).value?.features?.get("stealth")?.active)
    }
    @Test fun invalidUnsignedNeverTurnsIntoZero() {
        for (bad in listOf<Any>("", "00", "01", "-1", "1.0", " 1", "1 ", "18446744073709551616", "0", 1, JSONObject.NULL)) {
            val raw = fixtures.runtime()
            raw.getJSONObject("runtime").put("generation", bad)
            assertNull(runtime(raw).value, bad.toString())
        }
    }
    @Test fun missingNullAndWrongBooleanAreInvalidInsteadOfDisabled() {
        for (bad in listOf(JSONObject.NULL, "false", 0)) {
            val raw = fixtures.runtime()
            raw.getJSONObject("runtime").getJSONArray("features").getJSONObject(1).put("configured", bad)
            assertNull(runtime(raw).value)
        }
        val raw = fixtures.runtime()
        raw.getJSONObject("runtime").getJSONArray("features").getJSONObject(1).remove("active")
        assertNull(runtime(raw).value)
    }
    @Test fun unknownRuntimeIsSeparateFromUnsupported() {
        for ((status, expected) in listOf("unsupported" to SUMHRuntimeRead.Unsupported, "error" to SUMHRuntimeRead.Error,
            "unavailable" to SUMHRuntimeRead.Unavailable)) {
            val raw = fixtures.runtime().put("status", status).put("runtime", JSONObject.NULL)
            val parsed = runtime(raw).value!!
            assertEquals(expected, parsed.status)
            assertTrue(parsed.features.isEmpty())
            assertNull(parsed.generation)
        }
    }
    @Test fun futureMalformedAndTrailingDocumentsAreRejected() {
        assertNull(parse(fixtures.state().put("schema_version", 2)).value)
        for (text in listOf("{schema_version:1}", "{'schema_version':1}", fixtures.state().toString() + " {}",
            "{\"schema_version\":1,\"schema_version\":1}")) assertNull(SUMHControlStateParser.state(SUMHQueryReply(text)).value)
    }
    @Test fun duplicateOrMissingRuntimeFeatureFailsWithoutInventingState() {
        val raw = fixtures.runtime()
        val entries = raw.getJSONObject("runtime").getJSONArray("features")
        entries.put(entries.getJSONObject(0))
        assertNull(runtime(raw).value)
        entries.remove(entries.length() - 1)
        entries.remove(5)
        assertNull(runtime(raw).value)
    }
    @Test fun configuredInactiveIsStillAConfirmedSetting() {
        val raw = fixtures.state()
        raw.getJSONObject("runtime").getJSONObject("runtime").getJSONArray("features").getJSONObject(1)
            .put("configured", true).put("degraded", true)
        raw.getJSONObject("features").getJSONObject("stealth").put("reason", "configured_but_inactive")
        val parsed = fixtures.control(raw)
        assertTrue(parsed.runtime.value!!.features.getValue("stealth").configured)
        assertFalse(parsed.runtime.value!!.features.getValue("stealth").active)
        assertEquals(SUMHApplyAdvice.Verified, parsed.applyAdvice())
    }
    @Test fun comparisonDriftSurvivesAValidNonzeroEnvelope() {
        val envelope = JSONObject().put("ok", false).put("exit_code", 1).put("errno", 5)
            .put("stdout", fixtures.state("drift").toString()).put("stderr", "history unreadable")
        val parsed = SUMHControlStateParser.state(SUMHControlStateParser.reply(envelope.toString()))
        assertEquals(SUMHComparison.Drift, parsed.value!!.features.getValue("stealth").comparison)
        assertEquals("history unreadable", parsed.error)
        assertFalse(parsed.retained)
    }
    @Test fun malformedHistoryDoesNotEraseCurrentControls() {
        val raw = fixtures.state()
        raw.getJSONObject("history").put("status", "future")
        val parsed = fixtures.control(raw)
        assertEquals(SUMHComparison.Matches, parsed.state.value!!.features.getValue("stealth").comparison)
        assertEquals(SUMHHistoryRead.Invalid, parsed.state.value!!.history.status)
        assertEquals(SUMHApplyAdvice.Query, parsed.applyAdvice())
    }
    @Test fun reservationOnlyHistoryIsValidWithoutInventingExecutedOperation() {
        val raw = fixtures.state()
        raw.getJSONObject("history").put("status", "valid").put("last_operation", JSONObject.NULL)
        val parsed = fixtures.control(raw)
        val history = parsed.state.value!!.history
        assertEquals(SUMHHistoryRead.Valid, history.status)
        assertNull(history.error)
        assertNull(history.last)
        assertNull(parsed.state.error)
        assertEquals(SUMHComparison.Matches, parsed.state.value!!.features.getValue("stealth").comparison)
        assertEquals(SUMHApplyAdvice.Verified, parsed.applyAdvice())
    }
    @Test fun validNullRefreshClearsPreviouslyRetainedExecutedOperation() {
        val raw = fixtures.state()
        raw.getJSONObject("history").put("status", "valid").put("last_operation", fixtures.operation())
        val previous = fixtures.control(raw)
        assertNotNull(previous.state.value!!.history.last)
        raw.getJSONObject("history").put("last_operation", JSONObject.NULL)
        val refreshed = fixtures.control(raw).retaining(previous)
        assertEquals(SUMHHistoryRead.Valid, refreshed.state.value!!.history.status)
        assertNull(refreshed.state.value!!.history.last)
        assertFalse(refreshed.state.retained)
    }
    @Test fun preparedPhaseCannotMasqueradeAsLegacyCompletedOperation() {
        val raw = fixtures.state()
        raw.getJSONObject("history").put("status", "valid")
            .put("last_operation", fixtures.operation().put("phase", "prepared"))
        val parsed = fixtures.control(raw)
        assertEquals(SUMHHistoryRead.Invalid, parsed.state.value!!.history.status)
        assertNull(parsed.state.value!!.history.last)
        assertEquals(SUMHApplyAdvice.Query, parsed.applyAdvice())
        assertEquals(SUMHComparison.Matches, parsed.state.value!!.features.getValue("stealth").comparison)
    }
    @Test fun negativeTransportExitRetainsIndependentlyValidPayload() {
        val envelope = JSONObject().put("ok", false).put("exit_code", -110).put("errno", 110)
            .put("stdout", fixtures.state().toString()).put("stderr", "timeout after response")
        val parsed = SUMHControlStateParser.state(SUMHControlStateParser.reply(envelope.toString()))
        assertNotNull(parsed.value)
        assertEquals("timeout after response", parsed.error)
        val malformed = fixtures.state()
        malformed.put("history", JSONObject().put("status", "valid").put("error", "")
            .put("last_operation", fixtures.operation().put("exit_code", -1)))
        assertEquals(SUMHHistoryRead.Invalid, parse(malformed).value!!.history.status)
    }
    @Test fun unfinishedAndFailedOperationsAreNotSuccessful() {
        for (started in listOf(true, false)) {
            val raw = fixtures.state().put("history", JSONObject().put("status", "valid").put("error", "")
                .put("last_operation", fixtures.operation(started, if (started) 0 else 1)))
            val last = parse(raw).value!!.history.last!!
            assertEquals(if (started) SUMHOperationResult.Unknown else SUMHOperationResult.FailedOrPartial, last.result)
            if (started) assertEquals(SUMHApplyAdvice.Query, fixtures.control(raw).applyAdvice())
        }
    }
    @Test fun historicalVerificationCannotClaimAnotherBootApplied() {
        val raw = fixtures.state()
        val feature = raw.getJSONObject("features").getJSONObject("stealth")
        feature.put("last_verified", JSONObject().put("revision", fixtures.revision).put("boot_id", "old-boot")
            .put("instance_id", "22").put("generation", "2"))
        assertNotNull(parse(raw).value!!.features.getValue("stealth").lastVerified)
        feature.put("applied_revision", fixtures.revision)
        assertNull(parse(raw).value)
        feature.getJSONObject("last_verified").put("boot_id", "boot-a")
        assertEquals(fixtures.revision, parse(raw).value!!.features.getValue("stealth").appliedRevision)
    }
    @Test fun optionalQueriesAreNegotiatedAndFailuresReturnTypedUnknown() {
        val calls = mutableListOf<String>()
        val meta = JSONObject().put("runtime_status_schema", 1)
        val result = SUMHManager.readControlState(meta) { args -> calls.add(args.joinToString(" ")); SUMHQueryReply(fixtures.runtime().toString()) }
        assertEquals(listOf("api runtime"), calls)
        assertNull(result.state.value)
        assertNotNull(result.runtime.value)
        meta.put("state_ledger_schema", 1)
        val failed = SUMHManager.readControlState(meta) { error("transport unavailable") }
        assertNull(failed.state.value)
        assertNull(failed.runtime.value)
        assertEquals("transport unavailable", failed.state.error)
    }
    @Test fun validPartialStateUsesItsOwnRuntimeWithoutQueryingAnotherGeneration() {
        val calls = mutableListOf<String>()
        val result = SUMHManager.readControlState(JSONObject().put("state_ledger_schema", 1).put("runtime_status_schema", 1)) {
            calls.add(it.joinToString(" ")); SUMHQueryReply(fixtures.state().toString(), "journal read failed")
        }
        assertEquals(listOf("api state"), calls)
        assertEquals("journal read failed", result.state.error)
        assertNotNull(result.runtime.value)
        assertEquals(SUMHApplyAdvice.Query, result.applyAdvice())
    }
    @Test fun externalRefreshReplacesCurrentSampleAndFailureRetainsOnlyStaleFacts() {
        val original = fixtures.control()
        val updated = fixtures.control(fixtures.state("drift")).retaining(original)
        assertEquals(SUMHApplyAdvice.Drift, updated.applyAdvice())
        val failed = SUMHControlState(updated.capabilities, SUMHStateQuery(error = "offline"), SUMHStateQuery(error = "offline"))
            .retaining(updated)
        assertEquals(SUMHComparison.Drift, failed.state.value!!.features.getValue("stealth").comparison)
        assertTrue(failed.state.retained)
        assertEquals(SUMHApplyAdvice.Query, failed.applyAdvice())
        assertEquals(SUMHApplyAdvice.Verified, original.retaining(failed).applyAdvice())
        val noBoot = fixtures.state("drift").put("boot_id", JSONObject.NULL)
        noBoot.getJSONObject("runtime").put("boot_id", JSONObject.NULL)
        assertEquals(SUMHApplyAdvice.Query, fixtures.control(noBoot).applyAdvice())
    }
    @Test fun hideRegistrationAndUnknownInventoriesDoNotProveVisibility() {
        val raw = fixtures.state()
        raw.getJSONObject("features").getJSONObject("user_hide").put("status", "matches")
            .put("reason", "registered_definitions_verified_globally_suspended")
            .put("detail", JSONObject().put("verification_scope", "registered_definitions").put("binding_consistency", "per_rule_sampled")
                .put("pending_count", 0).put("error_count", 0).put("suspended_count", 2))
        val parsed = parse(raw).value!!
        assertEquals(2, parsed.features.getValue("user_hide").suspendedCount)
        assertEquals("registered_definitions", parsed.features.getValue("user_hide").verificationScope)
        for (name in listOf("modules", "maps_rules", "overlay_xattr_hide"))
            assertEquals(SUMHComparison.Unknown, parsed.features.getValue(name).comparison)
    }
}
