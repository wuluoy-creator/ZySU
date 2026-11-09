package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHMountConfigDialogTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private fun text(id: Int) = compose.activity.getString(id)
    private fun confirm() = compose.onNodeWithText(text(android.R.string.ok))

    @Test fun onlyMountModeIsEditableAndLegacyRulesStayOutsideTheDialog() {
        val initial = SUMHManager.ModuleInfo("module", "auto", listOf(ModuleRule("/system/legacy", "hide")))
        var requested: Pair<String, String>? = null
        var saved = false
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", initial, true, "auto", {}, { saved = true },
                onSave = { id, mode -> requested = id to mode })
        } }
        compose.onAllNodes(hasSetTextAction()).assertCountEquals(0)
        compose.onNodeWithText("/system/legacy").assertDoesNotExist()
        confirm().assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_mount_mode_sumh)).performClick()
        confirm().assertIsEnabled().performClick()
        compose.waitForIdle()
        assertEquals("module" to "sumh", requested)
        assertEquals(true, saved)
        assertEquals(listOf(ModuleRule("/system/legacy", "hide")), initial.rules)
    }

    @Test fun failedSaveRetainsModeAcrossStateRestoration() {
        val restoration = StateRestorationTester(compose)
        var requested: String? = null
        restoration.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", null, true, "auto", {}, {},
                onSave = { _, mode -> requested = mode; error("Write failed") })
        } }
        compose.onNodeWithText(text(R.string.sumh_mount_mode_sumh)).performClick()
        confirm().performClick()
        compose.waitForIdle()
        assertEquals("sumh", requested)
        compose.onNodeWithText("Write failed").assertIsDisplayed()
        restoration.emulateSavedInstanceStateRestore()
        compose.onNodeWithText(text(R.string.sumh_mount_mode_sumh)).assertIsSelected()
        confirm().assertIsEnabled()
    }

    @Test fun unsupportedKernelShowsSavedSumhModeButCannotSelectItAgain() {
        val initial = SUMHManager.ModuleInfo("module", "sumh", listOf(ModuleRule("/system/legacy", "hide")))
        var requested: String? = null
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", initial, false, "auto", {}, {},
                onSave = { _, mode -> requested = mode })
        } }
        compose.onNodeWithText(text(R.string.sumh_mount_mode_sumh)).assertIsNotEnabled()
        compose.onNodeWithText(text(R.string.sumh_mount_mode_overlay)).performClick()
        confirm().performClick()
        compose.waitForIdle()
        assertEquals("overlay", requested)
    }

    @Test
    @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun largeTextLandscapeKeepsModeAndSaveReachable() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            SUMHMountConfigDialog("module", "模块", null, true, "sumh", {}, {},
                onSave = { _, _ -> })
        } }
        compose.onNodeWithText(text(R.string.sumh_mount_mode_magic)).performScrollTo().performClick()
        confirm().assertIsDisplayed().assertIsEnabled()
        capture("module-mount-mode-landscape-large-text")
    }

    private fun capture(name: String) {
        val file = File("build/reports/sumh-layout/$name.png")
        file.parentFile?.mkdirs()
        val bitmap = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
