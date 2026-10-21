package com.zying.zysu.ui.sumh.util

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.runBlocking
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import kotlin.test.*

class SUMHHideTransferTest {
    private val revision = "sha256-v1:" + "a".repeat(64)
    private val boot = "test-boot"
    private fun row(number: Int = 1, path: String = "/test/" + number, enabled: Boolean = true,
        name: String = "", note: String = "", group: String = "") =
        SUMHHidePortableDefinition(number.toString(16).padStart(32, '0'), path, enabled, name, note, group)
    private fun choice(value: SUMHHideTransferResolution, id: String? = null) = SUMHHideTransferChoice(value, id)
    private fun review(saved: List<SUMHHidePortableDefinition> = emptyList(), imported: List<SUMHHidePortableDefinition>,
        choices: Map<String, SUMHHideTransferChoice> = emptyMap(), preserve: Boolean = false) =
        reviewSUMHHideImport(saved, imported, revision, boot, choices, preserve)
    private fun success(step: SUMHHideImportStep, previous: String, next: String = "sha256-v1:" + "b".repeat(64),
        binding: Int = 2): HideMutationResult {
        val runtime = if (step.after.enabled) HideRuntime("9", "0", 0, binding, if (binding == 3) -13 else if (binding != 2) -2 else 0) else null
        val outcome = if (runtime == null) "absent" else when (binding) { 2 -> "registered_bound"; 3 -> "registered_binding_error"; else -> "registered_pending" }
        return HideMutationResult(HideMutationReceipt("import-id", "applied", true, next, previous, boot, "", "", 1, 1, 0, 0, true,
            listOf(HideMutationItem(step.id, outcome, 0, true, runtime))), operationId = "f".repeat(32))
    }

    @Test fun portableRoundTripPreservesSixFieldsAndExactUnicode() {
        val rows = listOf(row(path = "/数据/😀\r\n\t ", enabled = false, name = " 名称 ", note = "\"\\\r\n\t\b\u000c", group = "--automatic\n"),
            row(2, "/é"), row(3, "/e\u0301"))
        val exported = exportSUMHHideDefinitions(rows)
        assertEquals(rows, importSUMHHideDefinitions(exported))
        assertFalse(exported.contains("boot_id")); assertFalse(exported.contains("generation")); assertFalse(exported.contains("revision"))
    }
    @Test fun canonicalEncodingMatchesNativeSortedKeysCommaSpacesAndNoColonSpace() {
        assertEquals("{\"kind\":\"sumh_user_hide_definitions\", \"rules\":[{\"enabled\":false, \"group\":\"\", " +
            "\"id\":\"00000000000000000000000000000001\", \"name\":\"\", \"note\":\"\\u0001\\n\", \"path\":\"/a\"}], \"schema_version\":1}\n",
            exportSUMHHideDefinitions(listOf(row(path = "/a", enabled = false, note = "\u0001\n"))))
        assertEquals("{\"kind\":\"sumh_user_hide_definitions\", \"rules\":[], \"schema_version\":1}\n", exportSUMHHideDefinitions(emptyList()))
    }
    @Test fun unknownFieldsMissingFieldsDuplicateKeysFutureTypesAndLegacyInputFail() {
        val original = exportSUMHHideDefinitions(listOf(row()))
        val invalid = listOf(original.replace("\"schema_version\":1", "\"schema_version\":2"),
            original.replace("\"schema_version\":1", "\"schema_version\":\"1\""),
            original.replace("\"schema_version\":1", "\"schema_version\":true"),
            original.replace("\"schema_version\":1", "\"schema_version\":1, \"schema_version\":1"),
            original.replace("\"rules\":", "\"runtime\":null, \"rules\":"),
            original.replace("\"enabled\":true", "\"enabled\":true, \"id\":\"" + row().id + "\""),
            original.replace("\"enabled\":true", "\"enabled\":\"true\""),
            original.replace("\"enabled\":true", "\"enabled\":null"),
            original.replace("\"note\":\"\", ", ""), original.replace("\"name\":\"\"", "\"name\":[]"),
            original + "{}", "[\"/a\"]", original.replace("\"kind\":\"sumh_user_hide_definitions\", ", ""), "\uFEFF" + original)
        invalid.forEach { assertFails("Accepted " + it) { importSUMHHideDefinitions(it) } }
        assertEquals(listOf(row()), importSUMHHideDefinitions(original.replace("\"schema_version\":1", "\"schema_version\":1.0")))
    }
    @Test fun duplicateIdentitiesAndPathsRejectWholeArtifact() {
        assertFails { exportSUMHHideDefinitions(listOf(row(), row(path = "/other"))) }
        assertFails { exportSUMHHideDefinitions(listOf(row(), row(2, row().path))) }
        val text = exportSUMHHideDefinitions(listOf(row(), row(2)))
        assertFails { importSUMHHideDefinitions(text.replace(row(2).id, row().id)) }
        assertFails { importSUMHHideDefinitions(text.replace(row(2).path, row().path)) }
    }
    @Test fun fieldLimitsUseUtf8AndPreserveAllowedControls() {
        val valid = row(path = "/" + "中".repeat(1364) + "aa", name = "😀".repeat(32), group = "中".repeat(42) + "xx", note = "😀".repeat(256))
        assertEquals(listOf(valid), importSUMHHideDefinitions(exportSUMHHideDefinitions(listOf(valid))))
        for (bad in listOf(valid.copy(path = valid.path + "x"), valid.copy(name = valid.name + "x"), valid.copy(group = valid.group + "x"),
            valid.copy(note = valid.note + "x"), valid.copy(path = "relative"), valid.copy(path = "/nul\u0000"), valid.copy(note = "\ud800"),
            valid.copy(path = "/\udc00"), valid.copy(id = "A".repeat(32)))) assertFails { exportSUMHHideDefinitions(listOf(bad)) }
        assertFails { importSUMHHideDefinitions(exportSUMHHideDefinitions(listOf(row())).replace("\"note\":\"\"", "\"note\":\"\\ud800\"")) }
    }
    @Test fun dedicatedParserAcceptsAll4096RowsBeyondTransportNodeBudget() {
        val rows = List(4096) { row(it + 1) }
        assertEquals(rows, importSUMHHideDefinitions(exportSUMHHideDefinitions(rows)))
        assertFails { exportSUMHHideDefinitions(rows + row(4097)) }
        assertFails { importSUMHHideDefinitions(" ".repeat(SUMH_HIDE_TRANSFER_BYTES) + "{}") }
    }
    private fun fullStoreRows(): List<SUMHHidePortableDefinition> {
        val rows = List(4096) { row(it + 1, enabled = false) }.toMutableList()
        val overhead = "\"kind\":\"sumh_user_hide_definitions\", ".toByteArray().size
        var remaining = SUMH_HIDE_STORE_BYTES - (exportSUMHHideDefinitions(rows).toByteArray().size - overhead)
        for (index in rows.indices) {
            val size = minOf(remaining, 1024); rows[index] = rows[index].copy(note = "a".repeat(size)); remaining -= size
        }
        assertEquals(0, remaining)
        return rows
    }
    @Test fun exactFourMiBStoreRoundTripsWithSeparatePortableEnvelopeBudget() {
        val rows = fullStoreRows(); val text = exportSUMHHideDefinitions(rows)
        assertTrue(text.toByteArray().size > SUMH_HIDE_STORE_BYTES)
        assertTrue(text.toByteArray().size <= SUMH_HIDE_TRANSFER_BYTES)
        assertEquals(rows, importSUMHHideDefinitions(text))
        val index = rows.indexOfFirst { it.note.length < 1024 }
        val over = rows.toMutableList(); over[index] = over[index].copy(note = over[index].note + "a")
        assertFails { exportSUMHHideDefinitions(over) }
        val compactOver = text.replace("\"name\":\"\"", "\"name\":\"x\"").replace(", ", ",")
        assertTrue(compactOver.toByteArray().size < SUMH_HIDE_TRANSFER_BYTES)
        assertFails { importSUMHHideDefinitions(compactOver) }
    }
    @Test fun selectedExportIsExactOrderedSubsetAndMissingSelectionFails() {
        val rows = listOf(row(), row(2, enabled = false, note = "保留"), row(3))
        assertEquals(listOf(rows[0], rows[2]), importSUMHHideDefinitions(exportSUMHHideSelectedDefinitions(rows, setOf(rows[2].id, rows[0].id))))
        assertTrue(importSUMHHideDefinitions(exportSUMHHideSelectedDefinitions(rows, emptySet())).isEmpty())
        assertFails { exportSUMHHideSelectedDefinitions(rows, setOf(row(4).id)) }
    }
    @Test fun defaultNewDisabledExistingEnabledPreservedAndInputUnmodified() {
        val old = row(enabled = true); val incoming = old.copy(enabled = false, note = "new"); val added = row(2, enabled = true)
        val result = review(listOf(old), listOf(incoming, added), mapOf(old.id to choice(SUMHHideTransferResolution.UseImported)))
        assertTrue(result.canApply); assertEquals(listOf(incoming.copy(enabled = true), added.copy(enabled = false)), result.candidate)
        assertEquals(1, result.addedCount); assertEquals(1, result.changedCount)
        assertEquals(0, result.enabledCount); assertEquals(0, result.disabledCount)
        assertFalse(result.rows[0].imported.enabled); assertTrue(result.rows[1].imported.enabled)
    }
    @Test fun explicitImportedEnabledPolicyCountsOnlyReviewedIdentities() {
        val old = row(enabled = true, group = "team"); val sibling = row(3, enabled = true, group = "team")
        val incoming = old.copy(enabled = false); val added = row(2, group = "team")
        val result = review(listOf(old, sibling), listOf(incoming, added), mapOf(old.id to choice(SUMHHideTransferResolution.UseImported)), true)
        assertEquals(1, result.enabledCount); assertEquals(1, result.disabledCount)
        assertEquals(sibling, result.candidate[1]); assertEquals(setOf(old.id, added.id), result.steps.map { it.id }.toSet())
    }
    @Test fun sameIdentityChangesRequireDecisionAndKeepPreservesWholeLocalDefinition() {
        val old = row(name = "saved"); val incoming = old.copy(name = "new", enabled = false)
        val pending = review(listOf(old), listOf(incoming))
        assertTrue(pending.unresolved); assertFalse(pending.canApply); assertEquals(listOf(old), pending.candidate)
        val kept = review(listOf(old), listOf(incoming), mapOf(old.id to choice(SUMHHideTransferResolution.KeepSaved)), true)
        assertTrue(kept.canApply); assertEquals(listOf(old), kept.candidate); assertTrue(kept.steps.isEmpty()); assertEquals(1, kept.skippedCount)
    }
    @Test fun samePathRequiresExplicitLocalIdentityAdoption() {
        val old = row(name = "local"); val incoming = row(2, old.path, false, name = "incoming")
        assertEquals(SUMHHideTransferCollision.PathCollision, review(listOf(old), listOf(incoming)).rows.single().collision)
        assertFalse(review(listOf(old), listOf(incoming), mapOf(incoming.id to choice(SUMHHideTransferResolution.UseImported))).canApply)
        val adopted = review(listOf(old), listOf(incoming), mapOf(incoming.id to choice(SUMHHideTransferResolution.AdoptLocalIdentity)))
        assertEquals(incoming.copy(id = old.id, enabled = true), adopted.candidate.single())
        assertEquals(old, adopted.steps.single().before); assertEquals(incoming.id, adopted.steps.single().sourceId)
    }
    @Test fun sameIdDifferentPathRequiresExplicitFreshPersistedIdentity() {
        val old = row(); val incoming = old.copy(path = "/new")
        val pending = review(listOf(old), listOf(incoming)); assertEquals(SUMHHideTransferCollision.IdCollision, pending.rows.single().collision)
        val newId = newSUMHHideTransferIdentity(incoming.id, pending.inputDigest, setOf(old.id))
        assertEquals(newId, newSUMHHideTransferIdentity(incoming.id, pending.inputDigest, setOf(old.id)))
        val copied = review(listOf(old), listOf(incoming), mapOf(incoming.id to choice(SUMHHideTransferResolution.ExplicitNewIdentity, newId)))
        assertTrue(copied.canApply); assertEquals(listOf(old, incoming.copy(id = newId, enabled = false)), copied.candidate)
        assertEquals(newId, copied.steps.single().id)
        assertFalse(review(listOf(old), listOf(incoming), mapOf(incoming.id to choice(SUMHHideTransferResolution.ExplicitNewIdentity, old.id))).canApply)
    }
    @Test fun crossedCollisionCanOnlySkipAndNeverDeletesEitherLocal() {
        val first = row(); val second = row(2); val incoming = first.copy(path = second.path)
        assertEquals(SUMHHideTransferCollision.CrossCollision, review(listOf(first, second), listOf(incoming)).rows.single().collision)
        for (resolution in listOf(SUMHHideTransferResolution.KeepSaved, SUMHHideTransferResolution.UseImported,
            SUMHHideTransferResolution.AdoptLocalIdentity, SUMHHideTransferResolution.ExplicitNewIdentity)) {
            val newId = row(3).id.takeIf { resolution == SUMHHideTransferResolution.ExplicitNewIdentity }
            assertFalse(review(listOf(first, second), listOf(incoming), mapOf(incoming.id to choice(resolution, newId))).canApply)
        }
        val skipped = review(listOf(first, second), listOf(incoming), mapOf(incoming.id to choice(SUMHHideTransferResolution.Skip)))
        assertTrue(skipped.canApply); assertEquals(listOf(first, second), skipped.candidate); assertTrue(skipped.steps.isEmpty())
    }
    @Test fun persistedCopyMappingRecognizesOnlyAnExactRebasedNoopNeverCrossedMutation() {
        val old = row(); val incoming = old.copy(path = "/new", name = "copy")
        val newId = newSUMHHideTransferIdentity(incoming.id, sumhHideTransferDigest(exportSUMHHideDefinitions(listOf(incoming))), setOf(old.id))
        val decisions = mapOf(incoming.id to choice(SUMHHideTransferResolution.ExplicitNewIdentity, newId))
        val appliedCopy = incoming.copy(id = newId, enabled = false)
        val result = review(listOf(old, appliedCopy), listOf(incoming), decisions)
        assertTrue(result.canApply); assertEquals(1, result.unchangedCount); assertTrue(result.steps.isEmpty())
        assertEquals(SUMHHideTransferCollision.CrossCollision, result.rows.single().collision)
        for (changed in listOf(appliedCopy.copy(name = "external"), appliedCopy.copy(note = "external"), appliedCopy.copy(group = "external"))) {
            val blocked = review(listOf(old, changed), listOf(incoming), decisions)
            assertFalse(blocked.canApply); assertTrue(blocked.steps.isEmpty()); assertEquals(listOf(old, changed), blocked.candidate)
        }
        val activationBlocked = review(listOf(old, appliedCopy), listOf(incoming), decisions, preserve = true)
        assertFalse(activationBlocked.canApply); assertTrue(activationBlocked.steps.isEmpty())
        assertFalse(review(listOf(old, appliedCopy), listOf(incoming)).canApply)
        assertFalse(review(listOf(old, appliedCopy), listOf(incoming),
            mapOf(incoming.id to choice(SUMHHideTransferResolution.AdoptLocalIdentity))).canApply)
        assertTrue(review(listOf(appliedCopy), listOf(incoming), decisions).canApply)
    }
    @Test fun duplicateResolvedTargetsAndUnexpectedChoiceIdsBlock() {
        val saved = listOf(row(), row(2)); val incoming = listOf(row(path = "/new1"), row(2, "/new2"))
        val choices = incoming.associate { it.id to choice(SUMHHideTransferResolution.ExplicitNewIdentity, row(3).id) }
        val result = review(saved, incoming, choices)
        assertFalse(result.canApply); assertEquals("hide_import_resolved_identity_collision", result.rows[1].error)
        assertFails { review(saved, incoming, mapOf(row(4).id to choice(SUMHHideTransferResolution.Skip))) }
        assertFalse(review(imported = listOf(row()), choices = mapOf(row().id to choice(SUMHHideTransferResolution.Skip, row(3).id))).canApply)
    }
    @Test fun mergePreservesSavedOrderAndEmptyOrSkippedInputCannotRemoveAnything() {
        val saved = listOf(row(), row(2)); val incoming = listOf(row(3), row(2).copy(name = "changed"))
        val result = review(saved, incoming, mapOf(row(2).id to choice(SUMHHideTransferResolution.UseImported)))
        assertEquals(listOf(saved[0], incoming[1], incoming[0].copy(enabled = false)), result.candidate)
        assertEquals(saved, review(saved, emptyList()).candidate)
        val skipped = review(saved, incoming, incoming.associate { it.id to choice(SUMHHideTransferResolution.Skip) })
        assertEquals(saved, skipped.candidate); assertTrue(skipped.steps.isEmpty())
    }
    @Test fun importedIdenticalRowsAreNoopsAndFullCandidateCapacityIsChecked() {
        val saved = List(4096) { row(it + 1) }
        val unchanged = review(saved, saved.take(2)); assertEquals(2, unchanged.unchangedCount); assertTrue(unchanged.steps.isEmpty())
        val overflow = review(saved, listOf(row(4097)))
        assertFalse(overflow.canApply); assertFalse(overflow.withinBudget); assertTrue(overflow.errors.contains("hide_import_store_budget"))
    }
    @Test fun ignoredEnabledOnlyDifferenceIsNoopAndDefaultNewRowsRebaseWithoutConflict() {
        val source = row(enabled = true); val saved = source.copy(enabled = false)
        val result = review(listOf(saved), listOf(source))
        assertTrue(result.canApply); assertFalse(result.unresolved); assertTrue(result.steps.isEmpty())
        assertEquals(1, result.unchangedCount); assertEquals(SUMHHideTransferCollision.Identical, result.rows.single().collision)
        val explicit = review(listOf(saved), listOf(source), preserve = true)
        assertTrue(explicit.unresolved); assertFalse(explicit.canApply)
        assertTrue(result.rows.single().imported.enabled)
    }
    @Test fun intermediateStoreBudgetBlocksGrowthBeforeShrinkEvenWhenFinalFits() {
        val saved = fullStoreRows(); val growIndex = saved.indexOfFirst { it.note.length < 1024 }
        val shrink = saved.first().copy(note = saved.first().note.dropLast(1))
        val grow = saved[growIndex].copy(note = saved[growIndex].note + "x")
        val decisions = listOf(grow, shrink).associate { it.id to choice(SUMHHideTransferResolution.UseImported) }
        val unsafe = review(saved, listOf(grow, shrink), decisions)
        assertFalse(unsafe.canApply); assertTrue(unsafe.errors.contains("hide_import_intermediate_store_budget"))
        assertFalse(unsafe.errors.contains("hide_import_store_budget"))
        assertTrue(review(saved, listOf(shrink, grow), decisions).canApply)
    }
    @Test fun enabledInvalidLeafBlocksActivationButDisabledPortableDefinitionSurvives() {
        for (path in listOf("/", "/trailing/", "/x/.", "/x/..", "/" + "中".repeat(86))) {
            val incoming = row(path = path)
            assertEquals(incoming, importSUMHHideDefinitions(exportSUMHHideDefinitions(listOf(incoming))).single())
            assertTrue(review(imported = listOf(incoming)).canApply)
            val active = review(imported = listOf(incoming), preserve = true)
            assertFalse(active.canApply); assertEquals("hide_import_unregistrable_leaf", active.rows.single().error)
        }
        assertNull(sumhHideActivationPathError("/not-yet-existing/file")); assertNull(sumhHideActivationPathError("/" + "中".repeat(85)))
    }
    @Test fun reviewTokenBindsRawInputBaselineBootChoicesAndEnabledPolicy() {
        val incoming = listOf(row()); val base = review(imported = incoming)
        val variants = listOf(review(imported = incoming, preserve = true),
            review(imported = incoming, choices = mapOf(row().id to choice(SUMHHideTransferResolution.Skip))),
            reviewSUMHHideImport(emptyList(), incoming, "sha256-v1:" + "b".repeat(64), boot),
            reviewSUMHHideImport(emptyList(), incoming, revision, "other-boot"),
            reviewSUMHHideImport(emptyList(), incoming, revision, boot, inputDigest = sumhHideTransferDigest(" " + exportSUMHHideDefinitions(incoming))),
            review(listOf(row(2)), incoming))
        variants.forEach { assertNotEquals(base.reviewToken, it.reviewToken) }
        assertEquals(base.reviewToken, review(imported = incoming).reviewToken)
    }
    @Test fun worstEscapedLegalRowFitsEveryUnchangedTransportBudgetAndExactPayload() {
        val incoming = row(path = "/" + "\u0001".repeat(4094), name = "\u0001".repeat(128), note = "\u0001".repeat(1024), group = "\u0001".repeat(128))
        val result = review(imported = listOf(incoming))
        assertTrue(result.canApply); val args = result.steps.single().arguments(revision)
        assertEquals(listOf("hide", "import-id", incoming.id, revision), args.take(4))
        assertTrue(args[4].contains("\"expect\":\"absent\"")); assertTrue(args[4].contains("\"enabled\":false"))
        assertTrue(sumhOperationInput(args).toByteArray().size <= 48 * 1024)
        val same = review(listOf(row()), listOf(row().copy(name = "new")), mapOf(row().id to choice(SUMHHideTransferResolution.UseImported)))
        assertTrue(same.steps.single().arguments(revision)[4].contains("\"expect\":\"same_identity\""))
    }
    @Test fun readerPreservesBytesClosesStreamsAndRejectsMalformedOversizeOrNoProgress() {
        val text = exportSUMHHideDefinitions(listOf(row(path = "/😀\n "))); var closed = false
        val stream = object : ByteArrayInputStream(text.toByteArray()) { override fun close() { closed = true; super.close() } }
        assertEquals(text, readSUMHHideDocument(stream)); assertTrue(closed)
        assertFails { readSUMHHideDocument(ByteArrayInputStream(byteArrayOf(0xc3.toByte(), 0x28))) }
        assertFails { readSUMHHideDocument(ByteArrayInputStream(ByteArray(SUMH_HIDE_TRANSFER_BYTES + 1))) }
        assertFails { readSUMHHideDocument(object : InputStream() { override fun read() = 0; override fun read(b: ByteArray, off: Int, len: Int) = 0 }) }
    }
    @Test fun writerValidatesBeforeAnyBytesAndPropagatesWriteOrCloseFailures() {
        val text = exportSUMHHideDefinitions(listOf(row()))
        val output = ByteArrayOutputStream(); writeSUMHHideDocument(output, text); assertEquals(text, output.toString("UTF-8"))
        val untouched = ByteArrayOutputStream(); assertFails { writeSUMHHideDocument(untouched, "[]") }; assertEquals(0, untouched.size())
        assertFailsWith<IOException> { writeSUMHHideDocument(object : OutputStream() { override fun write(b: Int) { throw IOException("write") } }, text) }
        assertFailsWith<IOException> { writeSUMHHideDocument(object : ByteArrayOutputStream() { override fun close() { throw IOException("close") } }, text) }
    }
    @Test fun batchChainsOnlyExactReceiptsAndKeepsOrderedSteps() = runBlocking<Unit> {
        val plan = review(imported = listOf(row(), row(2))); val seen = ArrayList<String>()
        val result = executeSUMHHideImport(plan, plan.reviewToken, { args, previous, currentBoot ->
            assertEquals(boot, currentBoot); assertEquals(previous, args[3]); seen += previous
            success(plan.steps[seen.size - 1], previous, "sha256-v1:" + seen.size.toString().repeat(64))
        })
        assertEquals(listOf(revision, "sha256-v1:" + "1".repeat(64)), seen)
        assertEquals(2, result.succeededCount); assertTrue(result.unattempted.isEmpty()); assertNull(result.stoppedReason)
        assertEquals("sha256-v1:" + "2".repeat(64), result.nextRevision)
    }
    @Test fun batchRequiresMatchingReviewAndNeverMutatesUnresolvedPlan() = runBlocking<Unit> {
        val valid = review(imported = listOf(row()))
        assertFails { executeSUMHHideImport(valid, "old", { _, _, _ -> error("must not run") }) }
        val pending = review(listOf(row()), listOf(row().copy(note = "new")))
        assertFails { executeSUMHHideImport(pending, pending.reviewToken, { _, _, _ -> error("must not run") }) }
    }
    @Test fun batchStopsAtSixteenAndFreshReviewMakesCompletedRowsNoops() = runBlocking<Unit> {
        val incoming = List(20) { row(it + 1, enabled = false) }; val plan = review(imported = incoming)
        var calls = 0
        val result = executeSUMHHideImport(plan, plan.reviewToken, { _, previous, _ -> success(plan.steps[calls++], previous) })
        assertEquals(16, calls); assertEquals(4, result.unattempted.size)
        assertEquals("hide_import_group_complete_rebase_required", result.stoppedReason)
        val fresh = review(incoming.take(16), incoming); assertEquals(16, fresh.unchangedCount); assertEquals(4, fresh.steps.size)
    }
    @Test fun batchDeadlineAndExplicitStopAreCheckedBeforeNextMutation() = runBlocking<Unit> {
        val plan = review(imported = listOf(row(), row(2))); var elapsed = 0L; var calls = 0
        val result = executeSUMHHideImport(plan, plan.reviewToken, { _, previous, _ ->
            elapsed = 30_000; success(plan.steps[calls++], previous)
        }, clock = { elapsed })
        assertEquals(1, calls); assertEquals(1, result.unattempted.size); assertEquals("hide_import_deadline_rebase_required", result.stoppedReason)
        val stopped = executeSUMHHideImport(plan, plan.reviewToken, { _, _, _ -> error("must not run") }, shouldContinue = { false })
        assertTrue(stopped.attempted.isEmpty()); assertEquals(2, stopped.unattempted.size)
    }
    @Test fun partialUnknownAndTransportExceptionsStopWithoutReplayOrRevisionAdvance() = runBlocking<Unit> {
        val plan = review(imported = listOf(row(), row(2))); val ok = success(plan.steps[0], revision)
        val failures = listOf(HideMutationResult(error = "unknown", operationId = "f".repeat(32)),
            ok.copy(value = ok.value!!.copy(status = "partial", confirmed = 0, failed = 1)))
        for (failure in failures) {
            var calls = 0
            val result = executeSUMHHideImport(plan, plan.reviewToken, { _, _, _ -> calls++; failure })
            assertEquals(1, calls); assertEquals(revision, result.nextRevision); assertEquals(1, result.unattempted.size)
            assertEquals(0, result.succeededCount)
        }
        val thrown = executeSUMHHideImport(plan, plan.reviewToken, { _, _, _ -> throw IOException("lost") })
        assertEquals(1, thrown.attempted.size); assertEquals(revision, thrown.nextRevision)
    }
    @Test fun mismatchedTypedReceiptCannotAdvanceRevision() = runBlocking<Unit> {
        val plan = review(imported = listOf(row(), row(2))); val ok = success(plan.steps[0], revision); val receipt = ok.value!!
        val bad = listOf(ok.copy(operationId = null), ok.copy(value = receipt.copy(action = "disable")),
            ok.copy(value = receipt.copy(bootId = "old")), ok.copy(value = receipt.copy(previousRevision = "sha256-v1:" + "c".repeat(64))),
            ok.copy(value = receipt.copy(revision = null)), ok.copy(value = receipt.copy(affected = 2)),
            ok.copy(value = receipt.copy(itemsComplete = false)), ok.copy(value = receipt.copy(confirmed = 0)),
            ok.copy(value = receipt.copy(items = listOf(receipt.items.single().copy(id = row(2).id)))),
            ok.copy(value = receipt.copy(items = listOf(receipt.items.single().copy(membership = null)))),
            ok.copy(value = receipt.copy(items = listOf(receipt.items.single().copy(outcome = "registered_bound", runtime = HideRuntime("2", "1", 0, 2, 0))))))
        for (value in bad) {
            val result = executeSUMHHideImport(plan, plan.reviewToken, { _, _, _ -> value })
            assertEquals(revision, result.nextRevision); assertEquals(0, result.succeededCount)
            assertEquals("hide_import_receipt_identity_unknown", result.attempted.single().second.error)
        }
    }
    @Test fun registeredPendingAndBindingErrorAreSuccessfulMembershipNotVisibility() = runBlocking<Unit> {
        for (binding in listOf(0, 1, 2, 3)) {
            val plan = review(imported = listOf(row()), preserve = true)
            val result = executeSUMHHideImport(plan, plan.reviewToken, { _, previous, _ -> success(plan.steps.single(), previous, binding = binding) })
            assertEquals(1, result.succeededCount)
            assertEquals(if (binding == 3) -13 else if (binding == 2) 0 else -2, result.attempted.single().second.value?.items?.single()?.runtime?.errno)
        }
    }
    @Test fun cancellationPropagatesAndNeverRetries() = runBlocking<Unit> {
        val plan = review(imported = listOf(row(), row(2))); var calls = 0
        assertFailsWith<CancellationException> {
            executeSUMHHideImport(plan, plan.reviewToken, { _, _, _ -> calls++; throw CancellationException("stop") })
        }
        assertEquals(1, calls)
    }
}
