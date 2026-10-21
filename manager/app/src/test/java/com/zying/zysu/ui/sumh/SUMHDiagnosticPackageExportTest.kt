package com.zying.zysu.ui.sumh

import android.app.Application
import androidx.activity.ComponentActivity
import androidx.compose.material3.Text
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowLooper
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.util.concurrent.atomic.AtomicInteger
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class SUMHDiagnosticPackageExportTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private fun owner() = SUMHDiagnosticPackageExport().also { state -> compose.setContent { Text(state.status.name) } }
    private fun finish(owner: SUMHDiagnosticPackageExport) {
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); !owner.busy }
        compose.waitForIdle()
    }

    @Test fun pickerCancellationDoesNotFetchOrCreateADestination() {
        val owner = owner(); val preview = SUMHDiagnosticPackageUiFixtures.preview()
        compose.runOnIdle { assertTrue(owner.begin(preview)); assertFalse(owner.begin(preview)); owner.cancel() }
        assertFalse(owner.busy); assertEquals(SUMHDiagnosticExportStatus.Idle, owner.status)
    }

    @Test fun exactReviewedBytesWrittenOnlyAfterConditionalRead() {
        val owner = owner(); val preview = SUMHDiagnosticPackageUiFixtures.preview("{\"text\":\"Unicode 中文 😀\"}\n")
        val output = ByteArrayOutputStream(); var queries = 0; val opens = AtomicInteger()
        compose.runOnIdle {
            assertTrue(owner.begin(preview))
            assertTrue(owner.write({ expected -> queries++; assertEquals(preview, expected); SUMHDiagnosticPackageUiFixtures.result(preview) },
                { opens.incrementAndGet(); output }))
        }
        finish(owner)
        assertEquals(1, queries); assertEquals(1, opens.get())
        assertContentEquals(preview.bytes(), output.toByteArray()); assertEquals(SUMHDiagnosticExportStatus.Saved, owner.status)
    }

    @Test fun lostProcessReviewCannotWriteARestoredPickerResult() {
        val owner = owner(); var queries = 0; var opens = 0
        compose.runOnIdle {
            assertFalse(owner.write({ queries++; SUMHDiagnosticPackageUiFixtures.result() }, { opens++; ByteArrayOutputStream() }))
        }
        assertEquals(0, queries); assertEquals(0, opens); assertEquals(SUMHDiagnosticExportStatus.Lost, owner.status)
    }

    @Test fun mismatchedBytesOrIdentityNeverOpenDestination() {
        val owner = owner(); val expected = SUMHDiagnosticPackageUiFixtures.preview(); val opens = AtomicInteger()
        for (actual in listOf(SUMHDiagnosticPackageUiFixtures.preview("{}\n"),
            SUMHDiagnosticPackageUiFixtures.preview(id = "b".repeat(32)), expected.copy(exportText = expected.exportText + " "))) {
            compose.runOnIdle {
                assertTrue(owner.begin(expected)); assertTrue(owner.write({ SUMHDiagnosticPackageUiFixtures.result(actual) },
                    { opens.incrementAndGet(); ByteArrayOutputStream() }))
            }
            finish(owner); assertEquals(SUMHDiagnosticExportStatus.Failed, owner.status)
        }
        assertEquals(0, opens.get())
    }

    @Test fun failedQueryDoesNotWriteAndCloseFailureDoesNotClaimSaved() {
        val owner = owner(); val expected = SUMHDiagnosticPackageUiFixtures.preview(); val opens = AtomicInteger()
        compose.runOnIdle {
            owner.begin(expected)
            owner.write({ SUMHDiagnosticPackageResult("export", expected.item.id, error = "diagnostic_result_unknown") },
                { opens.incrementAndGet(); ByteArrayOutputStream() })
        }
        finish(owner); assertEquals(0, opens.get()); assertEquals(SUMHDiagnosticExportStatus.Failed, owner.status)
        compose.runOnIdle {
            owner.begin(expected)
            owner.write({ SUMHDiagnosticPackageUiFixtures.result(expected) }, { object : ByteArrayOutputStream() {
                override fun close() { throw IOException("private failure text must not be shown") }
            } })
        }
        finish(owner); assertEquals(SUMHDiagnosticExportStatus.Failed, owner.status)
    }

    @Test fun concurrentCallbacksCannotDuplicateWriteAndBackgroundCompletionIsRetained() {
        val owner = owner(); val expected = SUMHDiagnosticPackageUiFixtures.preview()
        val gate = CompletableDeferred<SUMHDiagnosticPackageResult>(); val opens = AtomicInteger()
        compose.runOnIdle {
            owner.begin(expected)
            assertTrue(owner.write({ gate.await() }, { opens.incrementAndGet(); ByteArrayOutputStream() }))
            assertFalse(owner.write({ error("must not run") }, { error("must not open") }))
            owner.cancel(); assertTrue(owner.busy)
        }
        gate.complete(SUMHDiagnosticPackageUiFixtures.result(expected)); finish(owner)
        assertEquals(1, opens.get()); assertEquals(SUMHDiagnosticExportStatus.Saved, owner.status)
    }

    @Test fun fullContentChunksAreLosslessAndKeepSurrogatePairsAndTrailingLf() {
        val text = "x".repeat(2047) + "😀" + "中文".repeat(10000) + "\n"
        val chunks = diagnosticPackageTextChunks(text)
        assertEquals(text, chunks.joinToString(""))
        assertTrue(chunks.all { it.length <= 2048 && !it.first().isLowSurrogate() && !it.last().isHighSurrogate() })
        assertTrue(chunks.last().endsWith("\n"))
    }
}
