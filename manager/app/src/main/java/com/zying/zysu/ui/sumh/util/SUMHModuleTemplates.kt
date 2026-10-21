package com.zying.zysu.ui.sumh.util

import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import org.json.JSONObject

internal const val SUMH_TEMPLATE_INPUT_BYTES = 64 * 1024
internal enum class SUMHModuleTemplate { All, Exclude, Only, ExceptChildren, Hide }
internal enum class SUMHModuleTemplateError { PathsRequired, ChildOutsideParent }
internal fun moduleTemplatePathLabel(path: String): String = JSONObject.quote(path).replace("\\/", "/")
internal data class SUMHModuleTemplateResult(
    val rules: List<ModuleRule>? = null,
    val ruleError: SUMHModuleRuleError? = null,
    val templateError: SUMHModuleTemplateError? = null,
)

/** Line separators are input syntax; individual paths stay literal, including tabs and trailing spaces. */
internal fun moduleTemplateRules(template: SUMHModuleTemplate, paths: String, parent: String): SUMHModuleTemplateResult {
    val selected = if (paths.isEmpty()) emptyList() else paths.replace("\r\n", "\n").split('\n')
    if (template != SUMHModuleTemplate.All && selected.isEmpty())
        return SUMHModuleTemplateResult(templateError = SUMHModuleTemplateError.PathsRequired)
    if (template == SUMHModuleTemplate.ExceptChildren) {
        validateSUMHModuleRulePath(parent)?.let { return SUMHModuleTemplateResult(ruleError = it) }
    }
    val rules = when (template) {
        SUMHModuleTemplate.All -> emptyList()
        SUMHModuleTemplate.Exclude -> selected.map { ModuleRule(it, "none") }
        SUMHModuleTemplate.Only -> listOf(ModuleRule("/", "none")) + selected.map { ModuleRule(it, "sumh") }
        SUMHModuleTemplate.ExceptChildren -> listOf(ModuleRule(parent, "none")) + selected.map { ModuleRule(it, "sumh") }
        SUMHModuleTemplate.Hide -> selected.map { ModuleRule(it, "hide") }
    }
    validateSUMHModuleRules(rules)?.let { return SUMHModuleTemplateResult(ruleError = it) }
    if (template == SUMHModuleTemplate.ExceptChildren && selected.any {
        it == parent || !it.startsWith(if (parent == "/") "/" else "$parent/")
    }) return SUMHModuleTemplateResult(templateError = SUMHModuleTemplateError.ChildOutsideParent)
    return SUMHModuleTemplateResult(rules = rules)
}

internal data class SUMHModuleTemplateDiff(
    val added: List<ModuleRule>, val changed: List<Pair<ModuleRule, ModuleRule>>, val removed: List<ModuleRule>,
)

internal fun moduleTemplateDiff(before: List<ModuleRule>, after: List<ModuleRule>): SUMHModuleTemplateDiff {
    val prior = before.associateBy { it.path }
    val next = after.associateBy { it.path }
    return SUMHModuleTemplateDiff(after.filter { it.path !in prior },
        after.mapNotNull { rule -> prior[rule.path]?.takeIf { it != rule }?.let { it to rule } },
        before.filter { it.path !in next })
}
