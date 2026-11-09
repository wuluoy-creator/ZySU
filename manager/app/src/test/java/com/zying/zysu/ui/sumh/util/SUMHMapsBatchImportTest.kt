package com.zying.zysu.ui.sumh.util

import android.app.Application
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter.MapsPathRule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHMapsBatchImportTest {
    @Test
    fun preservesLiteralSpacesAndPreviewsAdditionsAndReplacements() {
        val existing = listOf(MapsPathRule("/old", "/before"), MapsPathRule("/keep", "/same"))
        val preview = previewSUMHMapsImport(
            "# comment\n\n/old\t/after \n/new \t/replacement \n/keep\t/same", existing)

        assertTrue(preview.canImport)
        assertEquals(1, preview.additions)
        assertEquals(listOf(MapsPathRule("/old", "/after ")), preview.replacements)
        assertEquals(listOf(MapsPathRule("/old", "/after "), existing[1],
            MapsPathRule("/new ", "/replacement ")), preview.candidate)
    }

    @Test
    fun reportsAllInvalidLinesAndDoesNotAllowPartialCommit() {
        val preview = previewSUMHMapsImport(
            "/good\t/replacement\nrelative\t/other\n/bad format\n/good\t/duplicate\n/extra\t/one\t/two", emptyList())

        assertFalse(preview.canImport)
        assertEquals(listOf(2, 3, 4, 5), preview.issues.map { it.line })
        assertEquals(listOf(
            SUMHMapsImportError.TargetAbsolute,
            SUMHMapsImportError.Format,
            SUMHMapsImportError.DuplicateTarget,
            SUMHMapsImportError.Format,
        ), preview.issues.map { it.error })
    }

    @Test
    fun enforcesCountAndEncodedBudgetOnWholeMergedSet() {
        val tooMany = previewSUMHMapsImport((1..257).joinToString("\n") { "/target$it\t/r" }, emptyList())
        assertTrue(tooMany.limitExceeded)
        assertFalse(tooMany.canImport)

        val large = List(15) { index -> "/${"a".repeat(3900)}$index\t/r" }.joinToString("\n")
        val overBudget = previewSUMHMapsImport(large, emptyList())
        assertTrue(overBudget.budgetExceeded)
        assertFalse(overBudget.canImport)
    }

    @Test
    fun noChangedRulesCannotBeImported() {
        val existing = listOf(MapsPathRule("/same", "/replacement"))
        val preview = previewSUMHMapsImport("/same\t/replacement", existing)
        assertFalse(preview.canImport)
        assertEquals(0, preview.additions)
        assertTrue(preview.replacements.isEmpty())
    }

    @Test
    fun validatesEachFieldWithItsOwnUtf8Limit() {
        val longTarget = "/" + "a".repeat(4095)
        val longReplacement = "/" + "界".repeat(85)
        val preview = previewSUMHMapsImport(
            "$longTarget\t/r\n/valid\t$longReplacement", emptyList())
        assertFalse(preview.canImport)
        assertEquals(listOf(SUMHMapsImportError.TargetTooLong,
            SUMHMapsImportError.ReplacementTooLong), preview.issues.map { it.error })
    }
}
