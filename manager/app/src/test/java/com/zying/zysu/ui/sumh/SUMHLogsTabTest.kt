package com.zying.zysu.ui.sumh

import android.app.Application
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHLogLevel
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import kotlin.math.abs
import kotlin.test.assertEquals
import kotlin.test.assertTrue

private class LogTabState(content: String = "") {
    val content = mutableStateOf(content)
    val kernel = mutableStateOf(false)
    val loading = mutableStateOf(false)
    val error = mutableStateOf<String?>(null)
    var refreshes = 0
    var sourceSwitches = 0
    var clears = 0
}

@Composable
private fun LogTabHarness(state: LogTabState, dark: Boolean = false) {
    MaterialTheme(colorScheme = if (dark) darkColorScheme() else androidx.compose.material3.lightColorScheme()) {
        Surface(Modifier.fillMaxSize()) {
            LogsTab(
                showKernelLog = state.kernel.value,
                onToggleLogType = {
                    state.sourceSwitches++
                    state.kernel.value = !state.kernel.value
                    // Match Workspace: invalidate old-source data before reading.
                    state.content.value = ""
                    state.loading.value = true
                    state.error.value = null
                },
                logContent = state.content.value,
                onRefreshLog = { state.refreshes++ },
                onClearLog = { state.clears++ },
                loading = state.loading.value,
                readError = state.error.value,
            )
        }
    }
}

/** Host UI regressions; these do not establish device frame timing or TalkBack behavior. */
@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHLogsTabTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }

    private fun text(id: Int) = compose.activity.getString(id)
    private fun show(state: LogTabState) = compose.setContent { LogTabHarness(state) }
    private fun list() = compose.onNodeWithTag("sumh_logs_list")
    private fun row(key: Int) = compose.onNodeWithTag("sumh_log_line_$key")
    private fun actions() = compose.onNodeWithContentDescription(text(R.string.sumh_logs_actions)).performClick()
    private fun search() = compose.onNode(hasSetTextAction() and hasText(text(R.string.sumh_logs_search)))
    private fun openSearch() = compose.onNodeWithContentDescription(text(R.string.sumh_logs_search)).performClick()
    private fun clipboard() = (compose.activity.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager)
        .primaryClip!!.getItemAt(0).text.toString()
    private fun copy() {
        actions()
        compose.onNodeWithTag("sumh_logs_copy").assertIsEnabled().performClick()
    }
    private fun ready() {
        // Default work returns through Android's Handler, independently of the Compose clock.
        compose.waitUntil(10000) {
            ShadowLooper.idleMainLooper()
            compose.onAllNodesWithTag("sumh_logs_processing").fetchSemanticsNodes().isEmpty()
        }
        compose.waitForIdle()
    }
    private fun daemon(level: String, message: String) = "2026-10-18 12:00:00 [$level] [test] $message"
    private fun lines(first: Int, last: Int) = (first..last).joinToString("\n") { "row-${it.toString().padStart(3, '0')}" }

    @Test fun pausedIsDefaultAndOnlyExplicitRefreshCallsReader() {
        val state = LogTabState("initial")
        show(state); ready()
        compose.onNodeWithTag("sumh_logs_follow").assertIsOff()
        assertEquals(0, state.refreshes)
        compose.mainClock.advanceTimeBy(30000)
        assertEquals(0, state.refreshes)
        compose.onNodeWithContentDescription(text(R.string.sumh_rules_refresh)).performClick()
        assertEquals(1, state.refreshes)
        row(1).assertTextEquals("initial")
    }

    @Test fun copyPreservesLiteralBlankTrailingLinesAndNormalizesLineSeparatorsOnly() {
        val state = LogTabState("alpha \r\n\r\n中文😀\rbeta\n")
        show(state); ready(); copy()
        assertEquals("alpha \n\n中文😀\nbeta\n", clipboard())
        row(1).assertTextEquals("alpha ")
        row(2).assertExists()
        row(5).assertExists()
    }

    @Test fun searchAndMultipleLevelsUsePrefixesAndCopyExactlyTheVisibleResult() {
        val info = daemon("INFO", "keep /system/ERROR-file")
        val warn = daemon("WARN", "keep warning")
        val error = daemon("ERROR", "keep error")
        val ordinary = "keep INFO ERROR body words"
        show(LogTabState(listOf(info, warn, error, ordinary, daemon("INFO", "other")).joinToString("\n")))
        ready(); openSearch(); search().performTextReplacement("keep"); ready()
        actions()
        compose.onNodeWithText(text(R.string.sumh_logs_filter_info)).performClick()
        compose.onNodeWithText(text(R.string.sumh_logs_filter_error)).performClick()
        ready()
        compose.onNodeWithText(text(R.string.sumh_logs_filter_info)).assertIsSelected()
        compose.onNodeWithText(text(R.string.sumh_logs_filter_error)).assertIsSelected()
        compose.onNodeWithTag("sumh_logs_copy").assertIsEnabled().performClick()
        row(1).assertTextEquals(info)
        row(3).assertTextEquals(error)
        row(2).assertDoesNotExist()
        row(4).assertDoesNotExist()
        assertEquals("$info\n$error", clipboard())
        actions(); compose.onNodeWithText(text(R.string.sumh_logs_filter_all)).performClick(); ready()
        row(4).assertTextEquals(ordinary)
        row(5).assertDoesNotExist()
        compose.onNodeWithContentDescription(text(R.string.close)).performClick(); ready()
        row(5).assertExists()
    }

    @Test fun severityIsAlsoExposedAsTextForClassifiedAndOrdinaryRows() {
        val info = daemon("INFO", "message")
        val plain = "/system/ERROR contains INFO in a body"
        show(LogTabState("$info\n$plain")); ready()
        row(1).assertTextEquals(info).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.StateDescription, text(R.string.sumh_logs_filter_info)))
        row(2).assertTextEquals(plain).assert(SemanticsMatcher.expectValue(
            SemanticsProperties.StateDescription, text(R.string.sumh_logs_level_unknown)))
    }

    @Test fun sourceSwitchInvalidatesOldRowsUntilTheNewManualReadFinishes() {
        val state = LogTabState("daemon-only")
        show(state); ready()
        compose.onNodeWithText(text(R.string.sumh_logs_daemon)).performClick()
        compose.onNodeWithText(text(R.string.sumh_logs_kernel)).performClick()
        ready()
        assertEquals(1, state.sourceSwitches)
        compose.onNodeWithText(text(R.string.sumh_logs_kernel)).assertIsDisplayed()
        compose.onNodeWithText("daemon-only").assertDoesNotExist()
        compose.onNodeWithTag("sumh_logs_loading").assertIsDisplayed()
        compose.onNodeWithTag("sumh_logs_empty").assertDoesNotExist()
        actions()
        compose.onNodeWithTag("sumh_logs_copy").assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_logs_clear)).assertIsNotEnabled()
        compose.runOnIdle {
            state.content.value = "<3>[ 12.345] KernelSU: sumh: kernel-only"
            state.loading.value = false
        }
        ready()
        compose.onNodeWithTag("sumh_logs_copy").assertIsEnabled().performClick()
        assertEquals("<3>[ 12.345] KernelSU: sumh: kernel-only", clipboard())
        compose.onNodeWithText("daemon-only").assertDoesNotExist()
    }

    @Test fun sameSourceLoadingAndFailureRetainSnapshotAndFailureIsNotEmptySuccess() {
        val state = LogTabState("retained snapshot")
        show(state); ready()
        compose.runOnIdle { state.loading.value = true }
        row(1).assertTextEquals("retained snapshot")
        compose.onNodeWithTag("sumh_logs_loading").assertIsDisplayed()
        compose.onNodeWithContentDescription(text(R.string.sumh_rules_refresh)).assertIsNotEnabled()
        actions(); compose.onNodeWithTag("sumh_logs_copy").assertIsNotEnabled()
        compose.runOnIdle { state.loading.value = false; state.error.value = "read_timeout" }
        compose.onNodeWithTag("sumh_logs_copy").assertIsEnabled().performClick()
        row(1).assertTextEquals("retained snapshot")
        compose.onNodeWithTag("sumh_logs_read_error").assertTextContains("read_timeout", substring = true)
        compose.onNodeWithTag("sumh_logs_empty").assertDoesNotExist()
        assertEquals("retained snapshot", clipboard())
    }

    @Test fun initialReadFailureHasNoSuccessfulEmptyPlaceholder() {
        val state = LogTabState().also { it.error.value = "permission_denied" }
        show(state); ready()
        compose.onNodeWithTag("sumh_logs_read_error").assertTextContains("permission_denied", substring = true)
        compose.onNodeWithTag("sumh_logs_empty").assertDoesNotExist()
        compose.runOnIdle { state.error.value = null }
        compose.onNodeWithTag("sumh_logs_empty").assertIsDisplayed()
    }

    @Test fun clearingDaemonRequiresConfirmationAndKernelActionIsDisabled() {
        val state = LogTabState("daemon row")
        show(state); ready(); actions()
        compose.onNodeWithText(text(R.string.sumh_logs_clear)).performClick()
        compose.onNodeWithText(text(R.string.sumh_logs_clear_confirm)).assertIsDisplayed()
        assertEquals(0, state.clears)
        compose.onNodeWithText(text(android.R.string.cancel)).performClick()
        assertEquals(0, state.clears)
        actions(); compose.onNodeWithText(text(R.string.sumh_logs_clear)).performClick()
        compose.onNodeWithText(text(android.R.string.ok)).performClick()
        assertEquals(1, state.clears)
        compose.onNode(isDialog()).assertDoesNotExist()
        compose.runOnIdle { state.kernel.value = true; state.content.value = "kernel row" }
        ready(); actions()
        compose.onNodeWithText(text(R.string.sumh_logs_clear)).assertIsNotEnabled()
        assertEquals(1, state.clears)
    }

    @Test fun searchFilterAndExplicitFollowSurviveSavedStateRestoration() {
        val restore = StateRestorationTester(compose)
        val keep = daemon("ERROR", "keep draft")
        val state = LogTabState(listOf(keep, daemon("INFO", "keep other"), daemon("ERROR", "discard")).joinToString("\n"))
        restore.setContent { LogTabHarness(state) }
        ready()
        compose.onNodeWithTag("sumh_logs_follow").performClick()
        openSearch(); search().performTextReplacement("keep"); ready()
        actions(); compose.onNodeWithText(text(R.string.sumh_logs_filter_error)).performClick(); ready()
        compose.onNodeWithTag("sumh_logs_copy").performClick()
        restore.emulateSavedInstanceStateRestore(); ready()
        search().assertTextContains("keep")
        compose.onNodeWithTag("sumh_logs_follow").assertIsOn()
        row(1).assertTextEquals(keep)
        row(2).assertDoesNotExist(); row(3).assertDoesNotExist()
        actions(); compose.onNodeWithText(text(R.string.sumh_logs_filter_error)).assertIsSelected()
        assertEquals(0, state.refreshes)
        assertEquals(0, state.clears)
    }

    @Test fun pausedAnchorSurvivesAppendedAndOverlappingTailRefreshes() {
        val state = LogTabState(lines(0, 159))
        show(state); ready()
        list().performScrollToIndex(60)
        val top = row(61).fetchSemanticsNode().boundsInRoot.top
        row(61).assertTextEquals("row-060")
        compose.runOnIdle { state.content.value = lines(0, 179) }; ready()
        row(61).assertIsDisplayed().assertTextEquals("row-060")
        assertTrue(abs(row(61).fetchSemanticsNode().boundsInRoot.top - top) <= 1f)
        compose.runOnIdle { state.content.value = lines(20, 179) }; ready()
        row(61).assertIsDisplayed().assertTextEquals("row-060")
        assertTrue(abs(row(61).fetchSemanticsNode().boundsInRoot.top - top) <= 1f)
        compose.onNodeWithTag("sumh_logs_follow").assertIsOff()
    }

    @Test fun followMovesToNewestAfterRefreshAndUserDragPausesIt() {
        val state = LogTabState(lines(0, 99))
        show(state); ready()
        compose.onNodeWithTag("sumh_logs_follow").performClick()
        row(100).assertIsDisplayed().assertTextEquals("row-099")
        compose.runOnIdle { state.content.value = lines(0, 119) }; ready()
        row(120).assertIsDisplayed().assertTextEquals("row-119")
        list().performTouchInput { swipeDown() }
        compose.onNodeWithTag("sumh_logs_follow").assertIsOff()
        compose.onNodeWithTag("sumh_logs_follow").performClick()
        row(120).assertIsDisplayed()
        assertEquals(0, state.refreshes)
    }

    @Test @Config(qualifiers = "zh-rCN-w320dp-h760dp-xhdpi")
    fun narrowChineseLargeTextKeepsActionsSearchFollowAndConsoleReachable() {
        RuntimeEnvironment.setFontScale(2f)
        val state = LogTabState("窄屏日志\n第二行")
        compose.setContent { LogTabHarness(state, dark = true) }; ready()
        compose.onNodeWithContentDescription(text(R.string.sumh_rules_refresh)).assertIsDisplayed().performClick()
        compose.onNodeWithTag("sumh_logs_follow").assertIsDisplayed().assertIsOff()
        row(1).assertIsDisplayed()
        assertFullSourceLabel(R.string.sumh_logs_daemon)
        capture("logs-narrow-large-text")
        openSearch(); search().assertIsDisplayed().performTextReplacement("窄屏"); ready()
        row(1).assertIsDisplayed()
        row(2).assertDoesNotExist()
        actions(); compose.onNodeWithTag("sumh_logs_copy").assertIsDisplayed().assertIsEnabled().performClick()
        assertEquals("窄屏日志", clipboard())
        assertEquals(1, state.refreshes)
        compose.onNodeWithContentDescription(text(R.string.close)).performClick(); ready()
        compose.onNodeWithText(text(R.string.sumh_logs_daemon)).performClick()
        compose.onNodeWithText(text(R.string.sumh_logs_kernel)).performClick()
        compose.runOnIdle { state.content.value = "<3>KernelSU: sumh: 内核日志"; state.loading.value = false }
        ready()
        assertFullSourceLabel(R.string.sumh_logs_kernel)
        capture("logs-narrow-kernel-large-text")
    }

    @Test fun consoleTextHasAtLeastFourPointFiveContrastForEverySeverity() {
        val background = SUMHLogPalette.surface.luminance().toDouble()
        val levels: List<SUMHLogLevel?> = SUMHLogLevel.entries + listOf(null)
        levels.forEach { level ->
            val foreground = SUMHLogPalette.text(level).luminance().toDouble()
            val ratio = (maxOf(foreground, background) + 0.05) / (minOf(foreground, background) + 0.05)
            assertTrue(ratio >= 4.5, "$level console contrast was $ratio")
        }
    }

    @Test @Config(qualifiers = "en-w760dp-h360dp-land-xhdpi")
    fun landscapeKeepsReaderAndControlsVisible() {
        val state = LogTabState(daemon("VERBOSE", "landscape console") + "\n" + daemon("ERROR", "second line"))
        show(state); ready()
        row(1).assertIsDisplayed()
        compose.onNodeWithTag("sumh_logs_follow").assertIsDisplayed()
        compose.onNodeWithContentDescription(text(R.string.sumh_rules_refresh)).assertIsDisplayed()
        capture("logs-landscape-light")
    }

    private fun capture(name: String) {
        val output = File("build/reports/sumh-layout/$name.png")
        output.parentFile?.mkdirs()
        output.outputStream().use {
            compose.onRoot().captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it)
        }
    }

    private fun assertFullSourceLabel(id: Int) {
        val layouts = mutableListOf<TextLayoutResult>()
        compose.onNodeWithText(text(id)).assertIsDisplayed()
            .performSemanticsAction(SemanticsActions.GetTextLayoutResult) { it(layouts) }
        assertTrue(layouts.isNotEmpty())
        assertTrue(layouts.none { it.hasVisualOverflow })
    }
}
