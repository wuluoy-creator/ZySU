package com.zying.zysu.ui.sumh.util

import android.app.Application
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHModulePreviewTest {
    private val f = SUMHModulePreviewFixtures
    private val saved get() = SUMHModulePreviewRequest.saved("m")
    private fun parse(raw: JSONObject, request: SUMHModulePreviewRequest = saved) = SUMHModulePreviewParser.parse(raw.toString(), request)
    private fun candidate(mode: String = "sumh", rules: List<SUMHManager.ModuleRule> = emptyList(), path: String = "") =
        SUMHModulePreviewRequest.candidate("m", mode, rules, path)

    @Test fun completeSavedResponseIsCompilationOnly() {
        val out = parse(f.saved())
        assertEquals(SUMHModulePreviewStatus.Complete, out.status)
        assertTrue(out.comparisonValid); assertNull(out.sample!!.candidate)
        assertEquals("18446744073709551615", out.sample.namespace)
        assertEquals("existing_source_write_through_not_read_only", out.sample.baseline.contributions.single().writePolicy)
        assertTrue("external_mount_owner" in out.unchecked)
    }
    @Test fun minimalUnknownAndInvalidAreNotEmptySuccessfulPlans() {
        for (status in listOf("unknown", "invalid")) {
            val result = parse(f.base(status).put("detail", f.bytes("failure\nreason")))
            assertNull(result.sample); assertFalse(result.comparisonValid); assertEquals("failure\nreason", result.detail!!.utf8)
        }
        assertFails { parse(f.base("complete")) }
        assertFails { parse(f.base().put("comparison_valid", true)) }
        assertFails { parse(f.base().put("baseline", f.plan())) }
    }
    @Test fun candidateUsesOneFullReplacementAndIndependentSavedBaseline() {
        val request = candidate("none", listOf(SUMHManager.ModuleRule("/", "none")))
        val result = parse(f.candidate(request), request).sample!!
        assertEquals(request.candidateRevision, result.candidateRevision)
        assertEquals(1, result.baseline.contributions.size); assertTrue(result.candidate!!.contributions.isEmpty())
        assertEquals("removed", result.diff!!.changes.single().change)
        assertEquals("none", result.after!!.modules.single().mode)
        assertEquals("/", result.after.modules.single().rules.single().path.utf8)
    }
    @Test fun emptyRulesMeanReplaceWithEmptyAndRequestDefensivelyCopies() {
        val rules = mutableListOf(SUMHManager.ModuleRule("/vendor/file ", "sumh"))
        val request = candidate(rules = rules)
        rules.clear()
        assertEquals(1, request.candidateRules!!.size)
        assertFails { (request.candidateRules as MutableList).clear() }
        val empty = candidate()
        val raw = JSONObject(empty.candidateJson!!)
        assertEquals(0, raw.getJSONArray("modules").getJSONObject(0).getJSONArray("rules").length())
        assertFalse(raw.has("config")); assertNull(saved.candidateJson)
    }
    @Test fun candidateDigestPathAndDefinitionAreAllBound() {
        val request = candidate(rules = listOf(SUMHManager.ModuleRule("/vendor/中文 ", "hide")), path = "/vendor/中文 ")
        parse(f.candidate(request), request)
        for (field in listOf("digest", "path", "mode", "rules", "target", "other", "global")) {
            val raw = f.candidate(request)
            when (field) {
                "digest" -> raw.put("candidate_revision", f.revision)
                "path" -> raw.getJSONObject("explain").put("requested_path", f.bytes("/vendor/中文"))
                "mode" -> raw.getJSONObject("configuration").getJSONObject("after").getJSONArray("modules").getJSONObject(0).put("mode", "none")
                "rules" -> raw.getJSONObject("configuration").getJSONObject("after").getJSONArray("modules").getJSONObject(0).put("rules", JSONArray())
                "target" -> raw.getJSONObject("configuration").put("changed_module_ids", JSONArray().put("other"))
                "other" -> raw.getJSONObject("configuration").getJSONObject("after").getJSONArray("modules").put(
                    JSONObject().put("id", f.bytes("other")).put("mode", "none").put("rules", JSONArray()))
                "global" -> raw.getJSONObject("configuration").getJSONObject("after").put("mount_backend", "sumh")
            }
            assertFails(field) { parse(raw, request) }
        }
    }
    @Test fun candidateAndSavedResponsesCannotBeInterchanged() {
        val request = candidate()
        assertFails { parse(f.candidate(request), saved) }
        assertFails { parse(f.saved(), request) }
        parse(f.candidate(request).also { it.getJSONObject("configuration").put("changed_module_ids", JSONArray()) }, request)
    }
    @Test fun unicodeLiteralSpacesTabsQuotesAndSlashesRoundTrip() {
        val path = "/vendor/中文\t \"quote\\slash "; val request = candidate(rules = listOf(SUMHManager.ModuleRule(path, "hide")), path = path)
        val input = JSONObject(request.candidateJson!!).getJSONArray("modules").getJSONObject(0)
        assertEquals(path, input.getJSONArray("rules").getJSONObject(0).getString("path"))
        assertEquals(path, parse(f.candidate(request), request).sample!!.requestedPath.utf8)
    }
    @Test fun invalidNewPathsModesIdsAndSurrogatesAreRejected() {
        for (path in listOf("relative", "/a/", "/a//b", "/a/./b", "/a/../b", "/a\n", "/a\r", "/a\u0000", "/\ud800", "/" + "x".repeat(4095))) {
            assertFails(path) { candidate(rules = listOf(SUMHManager.ModuleRule(path, "sumh"))) }
            assertFails(path) { SUMHModulePreviewRequest.saved("m", path) }
        }
        for (id in listOf("", ".", "..", "a/b", "中文", "a\n")) assertFails { SUMHModulePreviewRequest.saved(id) }
        assertFails { candidate("hide") }
        assertFails { candidate(rules = listOf(SUMHManager.ModuleRule("/a", "invalid"))) }
        assertFails { candidate(rules = listOf(SUMHManager.ModuleRule("/a", "sumh"), SUMHManager.ModuleRule("/a", "none"))) }
    }
    @Test fun newRuleAndFinalInputCapacitiesRemainIndependent() {
        val rules = List(256) { SUMHManager.ModuleRule("/p$it", "sumh") }
        assertEquals(256, candidate(rules = rules).candidateRules!!.size)
        assertFails { candidate(rules = rules + SUMHManager.ModuleRule("/last", "sumh")) }
        assertFails { candidate(rules = List(40) { SUMHManager.ModuleRule("/$it" + "x".repeat(1500), "sumh") }) }
        assertFails { SUMHModulePreviewRequest.candidate("m".repeat(SUMH_MODULE_PREVIEW_INPUT_BYTES), "sumh", emptyList()) }
    }
    @Test fun noncanonicalDuplicatedAndLargeSavedRuleArraysRemainReadable() {
        val raw = f.saved()
        val rules = JSONArray()
        repeat(600) { rules.put(JSONObject().put("path", f.bytes(if (it % 2 == 0) "/a/../b/" else "/same//path")).put("mode", "sumh")) }
        raw.getJSONObject("configuration").getJSONObject("before").getJSONArray("modules").getJSONObject(0).put("rules", rules)
        val out = parse(raw).sample!!.before.modules.single().rules
        assertEquals(600, out.size); assertEquals("/a/../b/", out[0].path.utf8)
    }
    @Test fun denseLegacyConfigurationNearTwoMiBExceedsOldNodeLimit() {
        val raw = f.saved()
        val entry = JSONObject().put("path", f.bytes("/a/")).put("mode", "sumh").toString()
        val count = (SUMH_MODULE_PREVIEW_RESPONSE_BYTES - raw.toString().toByteArray().size - 2048) / (entry.toByteArray().size + 1)
        val rules = JSONArray("[" + List(count) { entry }.joinToString(",") + "]")
        raw.getJSONObject("configuration").getJSONObject("before").getJSONArray("modules").getJSONObject(0).put("rules", rules)
        assertTrue(raw.toString().toByteArray().size > 1900 * 1024)
        assertEquals(count, parse(raw).sample!!.before.modules.single().rules.size)
    }
    @Test fun manyActualEventsAndDuplicatedExplainEventsFitDedicatedBudget() {
        val request = SUMHModulePreviewRequest.saved("m", "/vendor")
        val raw = f.saved("/vendor", count = 600)
        assertTrue(raw.toString().toByteArray().size < SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        val out = parse(raw, request).sample!!.baseline
        assertEquals(600, out.trace.size); assertEquals(out.trace, out.explainEvents)
    }
    @Test fun eventSubsetAndMappingAttributionCannotBeForged() {
        val raw = f.saved("/vendor")
        raw.getJSONObject("baseline").getJSONObject("explain").put("events", JSONArray())
        assertFails { parse(raw, SUMHModulePreviewRequest.saved("m", "/vendor")) }
        val mapping = f.saved(); mapping.getJSONObject("baseline").getJSONArray("trace").getJSONObject(0).put("mapping_index", 1)
        assertFails { parse(mapping) }
    }
    @Test fun failedCompilerTraceDoesNotRequireCommittedContributions() {
        val raw = f.partial(f.saved())
        raw.getJSONObject("baseline").put("compiled", false).put("ordered_contributions", JSONArray()).put("error", f.bytes("preparation failed"))
        val out = parse(raw).sample!!.baseline
        assertFalse(out.compiled); assertEquals(1, out.trace.size); assertTrue(out.contributions.isEmpty())
    }
    @Test fun ruleEventsCanHaveEmptyEffectiveModeAndDefaultMappingIndex() {
        val raw = f.saved()
        val event = f.event(action = "RULE").put("effective_mode", "").put("reason", "literal_overridden")
        raw.getJSONObject("baseline").getJSONArray("trace").put(event)
        assertEquals("", parse(raw).sample!!.baseline.trace.last().effectiveMode)
    }
    @Test fun exactHexBytesAndLargeDiagnosticsArePreserved() {
        val raw = f.saved()
        val hex = JSONObject().put("encoding", "hex").put("value", "2f736f757263652fff").put("byte_length", 9)
        raw.getJSONObject("baseline").getJSONArray("ordered_contributions").getJSONObject(0).put("source", hex)
        raw.getJSONObject("baseline").getJSONArray("trace").getJSONObject(0).put("source", hex)
        raw.getJSONObject("baseline").put("error", f.bytes("x".repeat(16000)))
        val out = parse(raw).sample!!.baseline
        assertNull(out.contributions.single().source!!.utf8); assertEquals("2f736f757263652fff", out.contributions.single().source!!.hex)
        assertEquals(16000, out.error.size)
    }
    @Test fun byteLengthNulAndMalformedEncodingAreRejected() {
        for (kind in listOf("length", "nul", "hex", "encoding", "surrogate")) {
            val raw = f.saved(); val path = raw.getJSONObject("baseline").getJSONArray("ordered_contributions").getJSONObject(0).getJSONObject("path")
            when (kind) {
                "length" -> path.put("byte_length", 1)
                "nul" -> path.put("value", "/a\u0000").put("byte_length", 3)
                "hex" -> path.put("encoding", "hex").put("value", "ABC").put("byte_length", 1)
                "encoding" -> path.put("encoding", "base64")
                "surrogate" -> path.put("value", "\ud800").put("byte_length", 1)
            }
            assertFails(kind) { parse(raw) }
        }
    }
    @Test fun hideAndMergeVariantsRetainHiddenAncestryAndWritePolicy() {
        val raw = f.saved()
        val plan = raw.getJSONObject("baseline")
        val merge = f.contribution(action = "MERGE").put("hidden_ancestors", JSONArray().put(f.bytes("/vendor")))
        plan.put("ordered_contributions", JSONArray().put(merge).put(f.contribution(1, "HIDE", "/vendor")))
            .put("trace", JSONArray().put(f.event(action = "MERGE").put("hidden_parent", f.bytes("/vendor"))))
        val out = parse(raw).sample!!.baseline.contributions
        assertEquals("/vendor", out[0].hiddenAncestors.single().utf8); assertNull(out[1].source); assertNull(out[1].writePolicy)
        val illegal = JSONObject(raw.toString()); illegal.getJSONObject("baseline").getJSONArray("ordered_contributions").getJSONObject(1).put("source", f.bytes("/source"))
        assertFails { parse(illegal) }
    }
    @Test fun staleResponseCannotRetainValidDiffOrCurrentFlags() {
        val request = candidate("none")
        val raw = f.partial(f.candidate(request), stale = true)
        assertEquals(SUMHModulePreviewStatus.Stale, parse(raw, request).status)
        val flag = JSONObject(raw.toString()).put("comparison_valid", true); assertFails { parse(flag, request) }
        raw.getJSONObject("diff").put("changes", f.candidate(request).getJSONObject("diff").getJSONArray("changes"))
        assertFails { parse(raw, request) }
    }
    @Test fun missingCandidateModuleLeavesUsefulButIncompleteDiff() {
        val request = candidate("none"); val raw = f.candidate(request)
        raw.put("status", "partial").put("reason", "planning_incomplete").put("comparison_valid", false)
            .put("candidate_modules_not_mountable", JSONArray().put("m"))
        raw.getJSONObject("candidate").put("valid_for_current_inputs", false)
        raw.getJSONObject("diff").put("complete", false)
        val out = parse(raw, request)
        assertTrue(out.sample!!.baseline.validForCurrentInputs); assertFalse(out.sample.candidate!!.validForCurrentInputs)
        assertEquals(1, out.sample.diff!!.changes.size)
    }
    @Test fun revisionFailureCannotBecomeBoundSuccess() {
        val request = candidate(); val raw = f.candidate(request)
        raw.put("status", "partial").put("reason", "planning_incomplete").put("comparison_valid", false).put("candidate_revision", JSONObject.NULL)
        raw.getJSONObject("baseline").put("valid_for_current_inputs", false)
        raw.getJSONObject("candidate").put("valid_for_current_inputs", false); raw.getJSONObject("diff").put("complete", false)
        assertFalse(parse(raw, request).comparisonValid)
        assertFails { parse(JSONObject(raw.toString()).put("status", "complete"), request) }
    }
    @Test fun unknownRuntimeMayKeepFirstSampleTokensAndDoesNotBlockCompilation() {
        val raw = f.saved(); raw.getJSONObject("runtime_context").put("known", false)
        val out = parse(raw)
        assertTrue(out.comparisonValid); assertFalse(out.sample!!.runtime.known); assertEquals("5", out.sample.runtime.instance)
        raw.getJSONObject("runtime_context").put("instance_id", JSONObject.NULL).put("generation", JSONObject.NULL)
        assertNull(parse(raw).sample!!.runtime.instance)
    }
    @Test fun validBootDocumentMayHaveInvalidMarkerFormat() {
        val raw = f.saved(); val owner = raw.getJSONObject("ownership")
        owner.put("boot_plan_status", "valid").put("boot_plan_revision", f.revision)
        owner.put("boot_plan", f.document().put("status", "invalid").put("current_boot", JSONObject.NULL).put("authorizes_apply", false)
            .put("module_ids", JSONArray()).put("format_valid", false))
        assertEquals("invalid", parse(raw).sample!!.ownership.bootPlan.document.status)
        owner.getJSONObject("boot_plan").put("status", "valid").put("format_valid", true).put("boot_id", "old-boot").put("current_boot", false)
        assertFalse(parse(raw).sample!!.ownership.bootPlan.currentBoot!!)
    }
    @Test fun otherBackendsAndEmptyExplainNeverProvePathAbsence() {
        val raw = f.saved("/not-observed", 0)
        raw.put("baseline", f.plan(0, selected = "magic"))
        val out = parse(raw, SUMHModulePreviewRequest.saved("m", "/not-observed"))
        assertTrue(out.comparisonValid); assertTrue(out.sample!!.baseline.otherBackendsUnplanned); assertTrue(out.sample.baseline.explainEvents.isEmpty())
    }
    @Test fun contradictoryCompletenessAuthorizationAndCapacityClaimsFail() {
        for (field in listOf("read_only", "requires_reboot", "apply_token", "complete", "compiled", "events", "observations", "revision", "owner", "authorize", "limit")) {
            val raw = f.saved(); val plan = raw.getJSONObject("baseline")
            when (field) {
                "read_only", "requires_reboot" -> raw.put(field, false)
                "apply_token" -> raw.put(field, "token")
                "complete", "compiled" -> plan.put(field, false)
                "events" -> plan.put("events_truncated", true)
                "observations" -> plan.put("observations_truncated", true)
                "revision" -> plan.put("source_revision", JSONObject.NULL)
                "owner" -> raw.getJSONObject("ownership").put("external_owner_absence_proven", true)
                "authorize" -> raw.getJSONObject("ownership").put("hot_apply_authorized", true)
                "limit" -> raw.getJSONObject("limits").put("response_bytes", 4194304)
            }
            assertFails(field) { parse(raw) }
        }
    }
    @Test fun strictJsonRejectsDuplicateEscapedKeysFutureSchemasCoercionAndTrailingData() {
        val base = f.base().toString()
        for (text in listOf("\uFEFF$base", "$base{}", "/*comment*/$base", base.replace("\"schema_version\":1", "\"schema_version\":1,\"schema\\u005fversion\":1")))
            assertFails { SUMHModulePreviewParser.parse(text, saved) }
        for (raw in listOf(f.base().put("schema_version", 2), f.base().put("schema_version", "1"), f.base().put("future", true), f.base().put("errno", true)))
            assertFails { parse(raw) }
        val action = f.saved(); action.getJSONObject("baseline").getJSONArray("trace").getJSONObject(0).put("action", "NEW_ACTION")
        assertFails { parse(action) }
    }
    @Test fun responseByteDepthAndNodeBudgetsRejectOversizedPayloads() {
        val base = f.base().toString()
        val exact = base + " ".repeat(SUMH_MODULE_PREVIEW_RESPONSE_BYTES - base.toByteArray().size)
        assertNull(SUMHModulePreviewParser.parse(exact, saved).sample)
        assertFails { SUMHModulePreviewParser.parse("$exact ", saved) }
        assertFails { SUMHModulePreviewParser.parse("{\"nested\":" + "[".repeat(20) + "0" + "]".repeat(20) + "}", saved) }
        assertFails { SUMHModulePreviewParser.parse("{\"nodes\":[" + "0,".repeat(262144) + "0]}", saved) }
    }
    @Test fun unsignedTokensAndByteRevisionsNeverUseFloatingPoint() {
        for (value in listOf("0", "01", "-1", "18446744073709551616", "1e3")) {
            val raw = f.saved().put("namespace_ino", value); assertFails { parse(raw) }
        }
        assertFails { parse(f.saved().put("config_revision", "sha256-v1:" + "A".repeat(64))) }
        val known = f.saved(); known.getJSONObject("runtime_context").put("instance_id", JSONObject.NULL); assertFails { parse(known) }
    }
    @Test fun capabilityNegotiationRequiresGranularFlagsAndBudgets() {
        val good = SUMHModulePreviewParser.capabilities(SUMHQueryReply(f.capabilities().toString()))
        assertTrue(good.saved && good.candidate && good.explain); assertNull(good.error)
        assertFalse(SUMHModulePreviewParser.capabilities(SUMHQueryReply("{}" )).saved)
        for (field in listOf("module_manager_preview_transport_schema", "module_plan_schema", "module_plan_input_bytes", "module_plan_response_bytes")) {
            val raw = f.capabilities().put(field, 2)
            assertFalse(SUMHModulePreviewParser.capabilities(SUMHQueryReply(raw.toString())).saved)
        }
        assertFalse(SUMHModulePreviewParser.capabilities(SUMHQueryReply(f.capabilities().toString(), "timeout")).candidate)
        assertFalse(SUMHModulePreviewParser.capabilities(SUMHQueryReply(f.capabilities().put("module_manager_inline_candidate", "true").toString())).candidate)
        val separate = SUMHModulePreviewParser.capabilities(SUMHQueryReply(f.capabilities().put("module_manager_inline_candidate", false).toString()))
        assertTrue(separate.saved); assertFalse(separate.candidate)
    }
    @Test fun dedicatedEnvelopeHandlesWorstEscapingWithoutChangingGenericLimits() {
        val payload = "\\".repeat(SUMH_MODULE_PREVIEW_RESPONSE_BYTES)
        val envelope = f.envelope(payload)
        assertTrue(envelope.size > 4 * 1024 * 1024)
        assertEquals(payload, SUMHModulePreviewParser.envelope(envelope).stdout)
        assertFails { SUMHControlStateParser.reply(String(envelope), SUMH_MODULE_PREVIEW_RESPONSE_BYTES) }
        assertFails { SUMHModulePreviewParser.envelope(f.envelope(payload + "x")) }
        assertFails { SUMHModulePreviewParser.envelope(byteArrayOf(0xc0.toByte(), 0xaf.toByte())) }
        assertFails { SUMHModulePreviewParser.envelope(f.envelope("{}\n")) }
    }
    @Test fun transportErrorAndContractErrorStaySeparateFromUsefulFacts() {
        val raw = f.saved().toString()
        val reply = SUMHModulePreviewParser.envelope(f.envelope(raw, ok = false))
        val result = SUMHModulePreviewParser.reply(reply, saved)
        assertNotNull(result.value); assertEquals("transport failed", result.error)
        val malformed = SUMHModulePreviewParser.reply(SUMHQueryReply("invalid", "timeout"), saved)
        assertNull(malformed.value); assertTrue(malformed.error!!.contains("timeout"))
    }
}
