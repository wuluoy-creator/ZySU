package com.zying.zysu.ui.sumh

import android.app.Application
import android.app.Instrumentation
import android.os.Bundle
import android.os.Parcel
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.*
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.R
import com.zying.zysu.ui.sumh.util.SUMHManager
import com.zying.zysu.ui.sumh.util.SUMHManager.ModuleRule
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.io.File
import java.io.IOException

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHEditorDraftLifecycleTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    @Test fun failuresKeepMemoryAndReadFailurePreventsWrites() {
        var failRead = true; var failWrite = true; var writes = 0
        var disk: EditorDraftSnapshot = mapOf("maps:" to MapsEditorDraft("/old ", "/r", MapsEditorDraft.baseline(null)).record())
        val storage = object : EditorDraftStorage {
            override fun read(): EditorDraftSnapshot { if (failRead) throw IOException("unknown schema"); return disk }
            override fun write(snapshot: EditorDraftSnapshot) { writes++; if (failWrite) throw IOException("full"); disk = snapshot }
        }
        lateinit var drafts: SUMHEditorDrafts
        compose.setContent { val scope = rememberCoroutineScope(); drafts = remember { SUMHEditorDrafts(scope, storage) } }
        compose.waitUntil(5000) { drafts.storageError }
        compose.runOnIdle { assertFalse(drafts.change("maps:") { null }); failRead = false; drafts.retry() }
        compose.waitUntil(5000) { drafts.loaded }
        assertEquals(0, writes)
        val changed = MapsEditorDraft("/中文 ", "/replacement ", MapsEditorDraft.baseline(null)).record()
        compose.runOnIdle { assertTrue(drafts.change("maps:") { changed }) }
        compose.waitUntil(5000) { drafts.storageError }
        assertEquals(changed, drafts.snapshot["maps:"])
        assertNotEquals(changed, disk["maps:"])
        compose.runOnIdle { failWrite = false; drafts.retry() }
        compose.waitUntil(5000) { !drafts.saving && !drafts.storageError }
        assertEquals(changed, disk["maps:"])
        compose.runOnIdle {
            assertFalse(drafts.change("maps:") { MapsEditorDraft("x".repeat(4096), "/r", MapsEditorDraft.baseline(null)).record() })
            assertEquals(changed, drafts.snapshot["maps:"])
        }
    }

    @Test fun manyModuleAndMapsDraftsStayOutOfRealSavedStateAndShareUpdates() {
        val directory = File(compose.activity.noBackupFilesDir, "sumh-drafts")
        val drafts = SUMHEditorDrafts.forDirectory(directory)
        val rules = List(256) { ModuleRule("/system/" + "a".repeat(150) + it, "sumh") }
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", SUMHManager.ModuleInfo("module", "sumh", rules),
                true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); drafts.loaded && "module:module" in drafts.snapshot }
        compose.runOnIdle {
            repeat(24) { index ->
                assertTrue(drafts.ensure("module:$index", ModuleEditorDraft("sumh", rules,
                    baseline = ModuleEditorDraft.baseline("sumh", rules)).record()))
            }
            repeat(256) { index -> assertTrue(drafts.ensure("maps:/$index", MapsEditorDraft("/" + "x".repeat(2000) + index,
                "/replacement ", MapsEditorDraft.baseline(null)).record())) }
            val another = SUMHEditorDrafts.forDirectory(directory)
            assertSame(drafts, another)
            val old = requireNotNull(drafts.snapshot["module:module"])
            another.change("module:module") { ModuleEditorDraft.from(requireNotNull(it)).copy(newPath = "/newer ").record() }
            drafts.forget("module:module", old)
            assertEquals("/newer ", ModuleEditorDraft.from(requireNotNull(drafts.snapshot["module:module"])).newPath)
        }
        compose.waitUntil(10000) { ShadowLooper.idleMainLooper(); !drafts.saving || drafts.storageError }
        assertFalse(drafts.storageFailure.toString(), drafts.storageError)
        assertEquals(drafts.snapshot, EditorDraftFile(directory).read())
        assertTrue(encodeEditorDrafts(drafts.snapshot).size > 1400 * 1024)
        val saved = Bundle()
        compose.runOnUiThread { Instrumentation().callActivityOnSaveInstanceState(compose.activity, saved) }
        val parcel = Parcel.obtain()
        try { parcel.writeBundle(saved); assertTrue("SavedState size: ${parcel.dataSize()}", parcel.dataSize() < 128 * 1024) }
        finally { parcel.recycle() }
    }

    @Test fun refreshedModuleConfigurationRequiresExplicitDecisionAndKeepsDraft() {
        val info = mutableStateOf(SUMHManager.ModuleInfo("module", "sumh", listOf(ModuleRule("/old", "sumh"))))
        compose.setContent { MaterialTheme {
            SUMHMountConfigDialog("module", "Module", info.value, true, "auto", {}, {}, onSave = { _, _, _ -> })
        } }
        val drafts = SUMHEditorDrafts.forDirectory(File(compose.activity.noBackupFilesDir, "sumh-drafts"))
        compose.waitUntil(5000) { ShadowLooper.idleMainLooper(); drafts.loaded && "module:module" in drafts.snapshot }
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_module_rules_title)).performScrollTo().performClick()
        val field = compose.onNode(hasSetTextAction() and hasText(compose.activity.getString(R.string.sumh_absolute_path)))
        field.performScrollTo().performTextReplacement("/unfinished ")
        compose.runOnIdle { info.value = info.value.copy(rules = listOf(ModuleRule("/external", "hide"))) }
        field.assertTextContains("/unfinished ")
        compose.onNodeWithText(compose.activity.getString(android.R.string.ok)).assertIsNotEnabled()
        compose.onNodeWithText(compose.activity.getString(R.string.sumh_editor_draft_keep)).performScrollTo().performClick()
        compose.onNodeWithText(compose.activity.getString(android.R.string.ok)).assertIsEnabled()
    }
}
