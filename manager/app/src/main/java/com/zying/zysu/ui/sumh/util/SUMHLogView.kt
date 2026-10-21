package com.zying.zysu.ui.sumh.util

internal enum class SUMHLogLevel { VERBOSE, DEBUG, INFO, WARN, ERROR }

internal data class SUMHLogLine(val key: Long, val text: String, val level: SUMHLogLevel?)
internal data class SUMHLogSnapshot(val lines: List<SUMHLogLine>, val nextKey: Long)
internal data class SUMHLogView(val lines: List<SUMHLogLine>, val copyText: String, val totalCount: Int)

/** No Android or coroutine dependency: the caller supplies its cancellation check. */
private class LogWork(private val check: () -> Unit) {
    private var remaining = 1024
    fun step() { if (--remaining == 0) { check(); remaining = 1024 } }
    fun checkpoint() = check()
    fun same(left: String, right: String): Boolean {
        step()
        if (left === right) return true
        if (left.length != right.length) return false
        for (index in left.indices) {
            step()
            if (left[index] != right[index]) return false
        }
        return true
    }
}

private val daemonLogPrefix = Regex(
    "^(?:(?:[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}|" +
        "early-boot (?:\\+[0-9]+\\.[0-9]{3}|\\(clock unset\\))) )?" +
        "\\[(VERBOSE|DEBUG|INFO|WARN|WARNING|ERROR)](?:[ \\t]|$)",
    RegexOption.IGNORE_CASE,
)

private val kernelLogPrefix = Regex(
    "^\\s*<([0-7])>\\s*(?:\\[\\s*\\d+(?:\\.\\d+)?]\\s*)?" +
        "(?:\\[\\s*[TC]\\d+]\\s*)?KernelSU: sumh:(?:\\s|$)",
)

private fun prefixTag(text: String, pattern: Regex, limit: Int): String? {
    val prefix = text.take(limit)
    val match = pattern.find(prefix) ?: return null
    // A truncated buffer must not manufacture the end-of-line alternative in
    // the header delimiter. A real whitespace delimiter remains sufficient.
    if (text.length > limit && match.range.last == prefix.lastIndex &&
        !prefix.last().isWhitespace()) {
        return pattern.find(text.take(limit + 1))?.groupValues?.get(1)
    }
    return match.groupValues[1]
}

private fun logLevel(text: String): SUMHLogLevel? {
    // core/log.cpp limits the timestamp to 64 bytes. Bound regex work to the
    // actual header; a long message must never be searched for severity words.
    val tag = prefixTag(text, daemonLogPrefix, 96)?.uppercase()
    if (tag != null) return if (tag == "WARNING") SUMHLogLevel.WARN else SUMHLogLevel.valueOf(tag)
    // Classification only needs the bounded header, never arbitrary message
    // text. A numeric marker in an ordinary daemon message is not severity.
    val priority = prefixTag(text, kernelLogPrefix, 256)?.singleOrNull()
    return when (priority) {
        '0', '1', '2', '3' -> SUMHLogLevel.ERROR
        '4' -> SUMHLogLevel.WARN
        '5', '6' -> SUMHLogLevel.INFO
        '7' -> SUMHLogLevel.DEBUG
        else -> null
    }
}

/** CR, LF and CRLF have lineSequence semantics, including the final blank. */
private fun splitLog(content: String, work: LogWork): List<String> {
    if (content.isEmpty()) return emptyList()
    val lines = ArrayList<String>()
    var start = 0
    var index = 0
    while (index < content.length) {
        work.step()
        val char = content[index]
        if (char == '\r' || char == '\n') {
            lines.add(content.substring(start, index))
            if (char == '\r' && index + 1 < content.length && content[index + 1] == '\n') index++
            start = index + 1
        }
        index++
    }
    lines.add(content.substring(start))
    return lines
}

/** Longest old suffix/new prefix, in linear line comparisons (KMP). */
private fun tailOverlap(old: List<SUMHLogLine>, oldSize: Int, text: List<String>, size: Int, work: LogWork): Int {
    if (oldSize == 0 || size == 0) return 0
    val failure = IntArray(size)
    for (index in 1 until size) {
        work.step()
        var match = failure[index - 1]
        while (match > 0 && !work.same(text[index], text[match])) match = failure[match - 1]
        if (work.same(text[index], text[match])) match++
        failure[index] = match
    }
    var match = 0
    for (index in 0 until oldSize) {
        work.step()
        while (match > 0 && (match == size || !work.same(old[index].text, text[match]))) match = failure[match - 1]
        if (work.same(old[index].text, text[match])) match++
    }
    // A delimiter-created blank by itself is not evidence of row identity.
    return if ((0 until match).any { work.step(); text[it].isNotEmpty() }) match else 0
}

internal fun parseSUMHLog(
    content: String,
    previous: SUMHLogSnapshot? = null,
    checkCancelled: () -> Unit = {},
): SUMHLogSnapshot {
    val work = LogWork(checkCancelled)
    work.checkpoint()
    val text = splitLog(content, work)
    val old = previous?.lines.orEmpty()
    var common = 0
    while (common < minOf(old.size, text.size) && work.same(old[common].text, text[common])) common++
    if (common == old.size && common == text.size && previous != null) {
        work.checkpoint()
        return previous
    }
    // The final empty item is the delimiter's sentinel, not a line that can
    // anchor rotation. Real internal blank lines still retain distinct keys.
    val oldSize = old.size - if (old.lastOrNull()?.text == "") 1 else 0
    val size = text.size - if (text.lastOrNull() == "") 1 else 0
    var kept = common
    var oldStart = 0
    if (common < oldSize) {
        val overlap = tailOverlap(old, oldSize, text, size, work)
        if (overlap > 0) {
            kept = overlap
            oldStart = oldSize - overlap
        } else if (!(0 until common).any { work.step(); text[it].isNotEmpty() }) {
            kept = 0
        }
    }
    var nextKey = previous?.nextKey ?: 1L
    val lines = ArrayList<SUMHLogLine>(text.size)
    text.forEachIndexed { index, line ->
        work.step()
        if (index < kept) lines.add(old[oldStart + index])
        else {
            check(nextKey > 0 && nextKey < Long.MAX_VALUE) { "Log line identity space exhausted" }
            lines.add(SUMHLogLine(nextKey++, line, logLevel(line)))
        }
    }
    work.checkpoint()
    return SUMHLogSnapshot(lines, nextKey)
}

/** Case-insensitive KMP matches Kotlin's UTF-16 Char ignoreCase semantics. */
private class LogSearch(search: String, private val work: LogWork) {
    private val needle = CharArray(search.length) { work.step(); fold(search[it]) }
    private val failure = IntArray(needle.size)
    init {
        for (index in 1 until needle.size) {
            work.step()
            var match = failure[index - 1]
            while (match > 0 && needle[index] != needle[match]) { work.step(); match = failure[match - 1] }
            if (needle[index] == needle[match]) match++
            failure[index] = match
        }
    }
    fun matches(text: String): Boolean {
        if (needle.isEmpty()) return true
        var match = 0
        for (char in text) {
            work.step()
            val folded = fold(char)
            while (match > 0 && folded != needle[match]) { work.step(); match = failure[match - 1] }
            if (folded == needle[match]) match++
            if (match == needle.size) return true
        }
        return false
    }
    private fun fold(char: Char): Char = char.uppercaseChar().lowercaseChar()
}

internal fun filterSUMHLog(
    snapshot: SUMHLogSnapshot,
    levels: Set<SUMHLogLevel>,
    search: String,
    checkCancelled: () -> Unit = {},
): SUMHLogView {
    val work = LogWork(checkCancelled)
    work.checkpoint()
    val matcher = LogSearch(search, work)
    val selected = ArrayList<SUMHLogLine>()
    val copy = StringBuilder()
    for (line in snapshot.lines) {
        work.step()
        if ((levels.isEmpty() || line.level in levels) && matcher.matches(line.text)) {
            if (selected.isNotEmpty()) copy.append('\n')
            selected.add(line)
            var start = 0
            while (start < line.text.length) {
                work.checkpoint()
                val end = minOf(start + 1024, line.text.length)
                copy.append(line.text, start, end)
                start = end
            }
        }
    }
    work.checkpoint()
    return SUMHLogView(selected, copy.toString(), snapshot.lines.size)
}
