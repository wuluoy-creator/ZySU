package com.zying.zysu.ui.sumh

import com.zying.zysu.ui.sumh.util.*

internal object SUMHDiagnosticPackageUiFixtures {
    val id = "a".repeat(32)
    fun capabilities() = SUMHDiagnosticPackageCapabilities(
        SUMHDiagnosticPackageSupport.Supported, SUMHDiagnosticPackageSupport.Supported,
        SUMHDiagnosticPackageSupport.Supported, SUMHDiagnosticPackageSupport.Supported,
        SUMHDiagnosticPackageSupport.Supported,
    )
    fun preview(text: String = "{\"sample\":\"example\"}\n", id: String = this.id,
        policy: String = "redacted", status: String = "partial"): SUMHDiagnosticPackagePreview {
        val bytes = diagnosticPackageUtf8(text)
        return SUMHDiagnosticPackagePreview(SUMHDiagnosticPackageItem(id, diagnosticPackageRevision(bytes), policy,
            status, "boot-protection", bytes.size), text)
    }
    fun result(preview: SUMHDiagnosticPackagePreview = preview(), action: String = "export") =
        SUMHDiagnosticPackageResult(action = action, id = preview.item.id, status = "observed", reason = "package_observed",
            exitCode = 0, revision = preview.item.revision, privacyPolicy = preview.item.privacyPolicy,
            diagnosticStatus = preview.item.diagnosticStatus, preview = preview)
}
