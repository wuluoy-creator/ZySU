package com.zying.zysu.ui.sumh.util

import android.app.Application
import com.zying.zysu.ui.sumh.*
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import org.junit.Test
import org.json.JSONTokener
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHModuleTemplatesTest {
    @Test fun fiveTemplatesProduceOnlyExistingHandwrittenModes() {
        assertEquals(emptyList(), moduleTemplateRules(SUMHModuleTemplate.All, "ignored", "").rules)
        assertEquals(listOf(ModuleRule("/vendor/etc/a", "none")), moduleTemplateRules(SUMHModuleTemplate.Exclude, "/vendor/etc/a", "").rules)
        assertEquals(listOf(ModuleRule("/", "none"), ModuleRule("/vendor/etc/a", "sumh")),
            moduleTemplateRules(SUMHModuleTemplate.Only, "/vendor/etc/a", "").rules)
        assertEquals(listOf(ModuleRule("/vendor/etc", "none"), ModuleRule("/vendor/etc/a", "sumh")),
            moduleTemplateRules(SUMHModuleTemplate.ExceptChildren, "/vendor/etc/a", "/vendor/etc").rules)
        assertEquals(listOf(ModuleRule("/vendor/etc/a", "hide")), moduleTemplateRules(SUMHModuleTemplate.Hide, "/vendor/etc/a", "").rules)
    }

    @Test fun exactUnicodeTabTrailingSpacesAndLineSeparatorsSurvive() {
        val literal = "/中文/\\/\"\t "
        assertEquals(literal, JSONTokener(moduleTemplatePathLabel(literal)).nextValue())
        assertEquals("\"/vendor/etc \"", moduleTemplatePathLabel("/vendor/etc "))
        val result = moduleTemplateRules(SUMHModuleTemplate.Hide, "/中文/😀 \r\n/vendor/\tfile ", "")
        assertEquals(listOf(ModuleRule("/中文/😀 ", "hide"), ModuleRule("/vendor/\tfile ", "hide")), result.rules)
        assertEquals(SUMHModuleRuleError.ControlCharacter, moduleTemplateRules(SUMHModuleTemplate.Hide, "/a\rb", "").ruleError)
        assertEquals(SUMHModuleRuleError.AbsolutePath, moduleTemplateRules(SUMHModuleTemplate.Exclude, "/a\n", "").ruleError)
        assertEquals(SUMHModuleRuleError.NonCanonicalPath, moduleTemplateRules(SUMHModuleTemplate.Only, "/a/../b", "").ruleError)
    }

    @Test fun emptyDuplicateAndGeneratedRootConflictsAreRejected() {
        assertEquals(SUMHModuleTemplateError.PathsRequired, moduleTemplateRules(SUMHModuleTemplate.Only, "", "").templateError)
        assertEquals(SUMHModuleRuleError.DuplicatePath, moduleTemplateRules(SUMHModuleTemplate.Hide, "/a\n/a", "").ruleError)
        assertEquals(SUMHModuleRuleError.DuplicatePath, moduleTemplateRules(SUMHModuleTemplate.Only, "/", "").ruleError)
    }

    @Test fun retainedChildrenMustBeStrictDescendantsWithComponentBoundary() {
        assertEquals(SUMHModuleTemplateError.ChildOutsideParent,
            moduleTemplateRules(SUMHModuleTemplate.ExceptChildren, "/vendor/etc2/a", "/vendor/etc").templateError)
        assertEquals(SUMHModuleRuleError.DuplicatePath,
            moduleTemplateRules(SUMHModuleTemplate.ExceptChildren, "/vendor/etc", "/vendor/etc").ruleError)
        assertEquals(listOf(ModuleRule("/", "none"), ModuleRule("/vendor", "sumh")),
            moduleTemplateRules(SUMHModuleTemplate.ExceptChildren, "/vendor", "/").rules)
        assertEquals(SUMHModuleRuleError.NonCanonicalPath,
            moduleTemplateRules(SUMHModuleTemplate.ExceptChildren, "/vendor/a", "/vendor/").ruleError)
    }

    @Test fun generatedRulesShareCountAndEncodedPayloadLimits() {
        val paths = List(256) { "/p$it" }.joinToString("\n")
        assertEquals(256, moduleTemplateRules(SUMHModuleTemplate.Hide, paths, "").rules?.size)
        assertEquals(SUMHModuleRuleError.RuleLimit, moduleTemplateRules(SUMHModuleTemplate.Only, paths, "").ruleError)
        val tooLarge = List(20) { "/$it" + "\\".repeat(1000) }.joinToString("\n")
        assertEquals(SUMHModuleRuleError.PayloadLimit, moduleTemplateRules(SUMHModuleTemplate.Exclude, tooLarge, "").ruleError)
        assertEquals(SUMHModuleRuleError.TooLong, moduleTemplateRules(SUMHModuleTemplate.Hide, "/" + "中".repeat(1400), "").ruleError)
    }

    @Test fun diffPreservesCandidateOrderAndReportsRemovalInsteadOfMerging() {
        val before = listOf(ModuleRule("/a", "hide"), ModuleRule("/b", "sumh"), ModuleRule("/c", "none"))
        val after = listOf(ModuleRule("/c", "none"), ModuleRule("/a", "sumh"), ModuleRule("/new", "hide"))
        val diff = moduleTemplateDiff(before, after)
        assertEquals(listOf(ModuleRule("/new", "hide")), diff.added)
        assertEquals(listOf(before[0] to after[1]), diff.changed)
        assertEquals(listOf(before[1]), diff.removed)
    }

    @Test fun privateTemplateDraftRoundTripsLargeSurrogateSafeText() {
        val draft = ModuleTemplateDraft(SUMHModuleTemplate.Only, "/😀 ".repeat(6000), "/parent ", "a".repeat(64))
        val record = draft.record()
        assertTrue(record.fields.all { it.toByteArray().size <= EDITOR_DRAFT_FIELD_BYTES })
        val snapshot = mapOf("module-template:test" to record)
        assertEquals(draft, ModuleTemplateDraft.from(decodeEditorDrafts(encodeEditorDrafts(snapshot)).getValue("module-template:test")))
        assertFalse(ModuleTemplateDraft.valid(draft.copy(paths = "x".repeat(SUMH_TEMPLATE_INPUT_BYTES + 1)).record()))
        assertFalse(ModuleTemplateDraft.valid(EditorDraftRecord(listOf("future", "", "a".repeat(64)))))
    }
}
