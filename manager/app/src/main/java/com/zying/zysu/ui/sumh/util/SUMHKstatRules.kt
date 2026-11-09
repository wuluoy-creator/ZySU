package com.zying.zysu.ui.sumh.util

import org.json.JSONArray
import org.json.JSONObject
import java.math.BigInteger

internal const val SUMH_MAX_KSTAT_RULES = 32
internal const val SUMH_MAX_KSTAT_ENCODED_BYTES = 56 * 1024
internal val SUMH_KSTAT_FIELDS = listOf("dev", "ino", "nlink", "size", "atime", "mtime", "ctime", "blksize", "blocks")
private val timeFields = setOf("atime", "mtime", "ctime")
private val unsignedPattern = Regex("0|[1-9][0-9]*")
private val signedPattern = Regex("0|-?[1-9][0-9]*")
private val u32Max = BigInteger("4294967295")
private val u64Max = BigInteger("18446744073709551615")
private val s64Max = BigInteger("9223372036854775807")
private val s64Min = BigInteger("-9223372036854775808")
private val nsecMax = BigInteger("999999999")

internal fun validSUMHKstatPath(path: String, limit: Int): Boolean =
    path.startsWith('/') && path.toByteArray(Charsets.UTF_8).size < limit &&
        path.none { it == '\u0000' || it == '\r' || it == '\n' }

internal fun validSUMHKstatNumber(field: String, value: String): Boolean {
    val signed = field.endsWith(".sec")
    if (value.length > 20 || !(if (signed) signedPattern else unsignedPattern).matches(value)) return false
    val number = value.toBigIntegerOrNull() ?: return false
    return when {
        field.endsWith(".nsec") -> number <= nsecMax
        signed -> number in s64Min..s64Max
        field == "ino" || field == "blocks" -> number <= u64Max
        field == "size" -> number <= s64Max
        else -> number <= u32Max
    }
}

internal fun validSUMHKstatCustomFields(fields: Map<String, String>): Boolean {
    if (fields.isEmpty()) return false
    val validKeys = SUMH_KSTAT_FIELDS.filterNot { it in timeFields }.toSet() +
        timeFields.flatMap { listOf("$it.sec", "$it.nsec") }
    if (fields.keys.any { it !in validKeys }) return false
    if (timeFields.any { ("$it.sec" in fields) != ("$it.nsec" in fields) }) return false
    return fields.all { (name, value) -> validSUMHKstatNumber(name, value) }
}

internal fun validSUMHKstatRules(rules: List<SUMHUiAdapter.KstatPathRule>): Boolean =
    rules.size <= SUMH_MAX_KSTAT_RULES &&
        rules.map { it.targetPath }.distinct().size == rules.size &&
        rules.all { rule ->
            validSUMHKstatPath(rule.targetPath, 256) && when (rule.mode) {
                "identity", "full" -> validSUMHKstatPath(rule.sourcePath, 4096) && rule.customFields.isEmpty()
                "custom" -> rule.sourcePath.isEmpty() && validSUMHKstatCustomFields(rule.customFields)
                else -> false
            }
        } && kstatRulesEncodedBytes(rules) <= SUMH_MAX_KSTAT_ENCODED_BYTES

internal fun parseKstatRule(item: JSONObject): SUMHUiAdapter.KstatPathRule {
    val mode = item.get("mode") as? String ?: error("Invalid Kstat mode")
    val expected = if (mode == "custom") setOf("target_path", "mode", "fields")
        else setOf("target_path", "source_path", "mode")
    require(item.keys().asSequence().toSet() == expected) { "Invalid Kstat rule fields" }
    val target = item.get("target_path") as? String ?: error("Invalid Kstat target")
    if (mode != "custom") return SUMHUiAdapter.KstatPathRule(
        target, item.get("source_path") as? String ?: error("Invalid Kstat source"), mode)
    val raw = item.get("fields") as? JSONObject ?: error("Invalid Kstat custom fields")
    val fields = mutableMapOf<String, String>()
    for (name in raw.keys()) {
        require(name in SUMH_KSTAT_FIELDS) { "Unknown Kstat field" }
        if (name in timeFields) {
            val time = raw.get(name) as? JSONObject ?: error("Invalid Kstat time")
            require(time.keys().asSequence().toSet() == setOf("sec", "nsec")) { "Invalid Kstat time" }
            fields["$name.sec"] = time.get("sec") as? String ?: error("Invalid Kstat time seconds")
            fields["$name.nsec"] = time.get("nsec") as? String ?: error("Invalid Kstat time nanoseconds")
        } else fields[name] = raw.get(name) as? String ?: error("Invalid Kstat value")
    }
    return SUMHUiAdapter.KstatPathRule(target, "", mode, fields)
}

internal fun kstatRulesJson(rules: List<SUMHUiAdapter.KstatPathRule>): JSONArray = JSONArray().also { array ->
    rules.forEach { rule ->
        val item = JSONObject().put("target_path", rule.targetPath).put("mode", rule.mode)
        if (rule.mode == "custom") {
            val fields = JSONObject()
            for (name in SUMH_KSTAT_FIELDS) {
                if (name in timeFields) {
                    val sec = rule.customFields["$name.sec"] ?: continue
                    fields.put(name, JSONObject().put("sec", sec).put("nsec", rule.customFields["$name.nsec"]))
                } else rule.customFields[name]?.let { fields.put(name, it) }
            }
            item.put("fields", fields)
        } else item.put("source_path", rule.sourcePath)
        array.put(item)
    }
}

internal fun kstatRulesEncodedBytes(rules: List<SUMHUiAdapter.KstatPathRule>): Int =
    JSONObject.quote(kstatRulesJson(rules).toString()).toByteArray(Charsets.UTF_8).size +
        3 * rules.sumOf { rule -> rule.targetPath.count { it == '/' } + rule.sourcePath.count { it == '/' } }
