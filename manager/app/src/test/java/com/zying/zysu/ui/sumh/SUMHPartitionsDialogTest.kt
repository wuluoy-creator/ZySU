package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.StateRestorationTester
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.ui.sumh.util.*
import kotlinx.coroutines.CompletableDeferred
import org.junit.After
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "w360dp-h760dp-xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class SUMHPartitionsDialogTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFontScale() { RuntimeEnvironment.setFontScale(1f) }
    private val f = SUMHPartitionsFixtures
    private fun discover() = compose.onNodeWithTag("partitions_discover").performScrollTo().performClick()
    private fun select(name: String = "extra") = compose.onNodeWithTag("partition_$name").performScrollTo().performClick()
    private fun review() = compose.onNodeWithTag("partitions_review").performScrollTo().performClick()
    private fun save() = compose.onNodeWithTag("partitions_save")
    private fun drafts() = SUMHEditorDrafts.forDirectory(File(compose.activity.noBackupFilesDir, "sumh-drafts"))

    @Test fun discoveryAndReviewDoNotSaveAndSuccessIsSeparateReceipt() {
        var discoveries = 0; var saves = 0; var published: List<String>? = null
        compose.setContent { MaterialTheme {
            SUMHPartitionsDialog(true, "config", { published = it }, {}, {},
                discover = { discoveries++; SUMHPartitionDiscoveryResult(f.sample()) },
                save = { revision, names ->
                    saves++; assertEquals(f.revision, revision); assertEquals(listOf("old_unknown", "extra"), names)
                    SUMHPartitionSaveResult(SUMHPartitionsParser.save(f.save().toString()))
                })
        } }
        assertEquals(0, discoveries); save().assertIsNotEnabled()
        discover(); select(); save().assertIsNotEnabled(); review()
        assertEquals(0, saves); save().assertIsEnabled().performClick()
        compose.onNodeWithTag("partitions_saved").performScrollTo().assertIsDisplayed()
        assertEquals(1, saves); assertEquals(listOf("old_unknown", "extra"), published)
        save().assertIsNotEnabled()
    }

    @Test fun unknownSaveRetainsSelectionAndCannotRepeatWithoutDiscovery() {
        var saves = 0
        compose.setContent { MaterialTheme {
            SUMHPartitionsDialog(true, "config", {}, {}, {},
                discover = { SUMHPartitionDiscoveryResult(f.sample()) },
                save = { _, _ -> saves++; SUMHPartitionSaveResult(error = "timeout") })
        } }
        discover(); select(); select("old_unknown"); review(); save().performClick()
        compose.onNodeWithText("timeout").performScrollTo().assertIsDisplayed()
        save().assertIsNotEnabled(); assertEquals(1, saves)
        assertEquals(listOf("extra"), PartitionsDraft.from(drafts().snapshot.getValue(PARTITIONS_DRAFT_KEY)).selected)
        discover(); save().assertIsNotEnabled(); review(); save().assertIsEnabled()
        assertEquals(1, saves)
    }

    @Test fun restorationPreservesSelectionButRequiresNewObservationAndReview() {
        val restoration = StateRestorationTester(compose)
        var saves = 0
        restoration.setContent { MaterialTheme {
            SUMHPartitionsDialog(true, "config", {}, {}, {},
                discover = { SUMHPartitionDiscoveryResult(f.sample()) },
                save = { _, _ -> saves++; SUMHPartitionSaveResult(error = "unused") })
        } }
        discover(); select(); review(); save().assertIsEnabled()
        restoration.emulateSavedInstanceStateRestore(); save().assertIsNotEnabled()
        discover(); compose.onNodeWithTag("partition_extra").performScrollTo().assertIsOn()
        save().assertIsNotEnabled(); assertEquals(0, saves)
    }

    @Test fun externalChangeDuringDiscoveryCannotReplaceAnExistingDraft() {
        val fingerprint = mutableStateOf("one")
        val response = CompletableDeferred<SUMHPartitionDiscoveryResult>()
        compose.setContent { MaterialTheme {
            SUMHPartitionsDialog(true, fingerprint.value, {}, {}, {}, discover = { response.await() })
        } }
        discover()
        compose.runOnIdle { fingerprint.value = "two" }
        compose.waitForIdle()
        compose.runOnIdle { response.complete(SUMHPartitionDiscoveryResult(f.sample())) }
        save().assertIsNotEnabled()
        assertNull(drafts().snapshot[PARTITIONS_DRAFT_KEY])
    }

    @Test fun externalSavedListNeedsExplicitRebaseAndNewReview() {
        var calls = 0
        compose.setContent { MaterialTheme {
            SUMHPartitionsDialog(true, "config", {}, {}, {}, discover = {
                calls++; SUMHPartitionDiscoveryResult(if (calls == 1) f.sample() else
                    f.sample().copy(revision = f.nextRevision, saved = listOf("old_unknown", "external")))
            })
        } }
        discover(); select(); select("old_unknown"); discover()
        save().assertIsNotEnabled()
        compose.onNodeWithTag("partitions_rebase").performScrollTo().performClick()
        review(); save().assertIsEnabled()
        assertEquals(listOf("external", "extra"), PartitionsDraft.from(drafts().snapshot.getValue(PARTITIONS_DRAFT_KEY)).selected)
    }

    @Test fun vanishedNewCandidateCanBeRemovedFromRetainedDraft() {
        var calls = 0
        compose.setContent { MaterialTheme {
            SUMHPartitionsDialog(true, "config", {}, {}, {}, discover = {
                calls++; SUMHPartitionDiscoveryResult(if (calls == 1) f.sample() else f.sample().copy(candidates = emptyList()))
            })
        } }
        discover(); select(); select("old_unknown"); discover()
        compose.onNodeWithTag("partitions_review").performScrollTo().assertIsNotEnabled()
        select("extra"); review(); save().assertIsEnabled()
        assertEquals(emptyList(), PartitionsDraft.from(drafts().snapshot.getValue(PARTITIONS_DRAFT_KEY)).selected)
    }

    @Test @Config(qualifiers = "zh-rCN-w360dp-h760dp-xhdpi")
    fun chineseLargeTextKeepsReviewAndSaveVisible() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme { SUMHPartitionsDialog(true, "config", {}, {}, {}, discover = { SUMHPartitionDiscoveryResult(f.sample()) }) } }
        discover(); select(); review()
        compose.onNodeWithTag("partitions_diff").performScrollTo().assertIsDisplayed()
        save().assertIsDisplayed().assertIsEnabled(); capture("partitions-narrow-large-text")
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun landscapeLargeTextRetainsFailureAndFixedActions() {
        RuntimeEnvironment.setFontScale(2f)
        compose.setContent { MaterialTheme(colorScheme = darkColorScheme()) {
            SUMHPartitionsDialog(true, "config", {}, {}, {}, discover = { SUMHPartitionDiscoveryResult(f.sample()) },
                save = { _, _ -> SUMHPartitionSaveResult(error = "timeout") })
        } }
        discover(); select(); review(); save().performClick()
        compose.onNodeWithText("timeout").performScrollTo().assertIsDisplayed()
        save().assertIsDisplayed().assertIsNotEnabled(); capture("partitions-landscape-large-text")
    }

    private fun capture(name: String) {
        val file = File("build/reports/sumh-layout/$name.png"); file.parentFile?.mkdirs()
        val bitmap = compose.onNode(isDialog()).captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
