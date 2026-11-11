package com.zying.zysu.ui.viewmodel

import com.zying.zysu.ui.util.getRootShell
import com.zying.zysu.ui.util.ksudCmd
import org.json.JSONArray
import org.json.JSONTokener

/** A failed query must never replace the last module snapshot with a fabricated empty list. */
internal fun queryModuleList(): JSONArray {
    val result = getRootShell().newJob().add(ksudCmd("module list"))
        .to(ArrayList<String>(), null).exec()
    return parseModuleListResponse(result.isSuccess, result.out.joinToString("\n"))
}

internal fun parseModuleListResponse(success: Boolean, output: String): JSONArray {
    check(success) { "Module list command failed" }
    check(output.isNotBlank()) { "Module list command returned no data" }
    val tokens = JSONTokener(output)
    val modules = tokens.nextValue()
    check(modules is JSONArray && tokens.nextClean() == '\u0000') { "Invalid module list response" }
    return modules
}
