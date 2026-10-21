package com.zying.zysu.ui.sumh

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.*
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import com.zying.zysu.ui.sumh.util.*
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
class SUMHProfilesScreenTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    @After fun resetFont() { RuntimeEnvironment.setFontScale(1f) }
    private val revision = "sha256-v1:" + "a".repeat(64)
    private val profileRevision = "sha256-v1:" + "b".repeat(64)
    private val item = SUMHProfileItem("profile-one", "Before module update", "Saved intent, not module files", profileRevision)
    private val state = mutableStateOf(SUMHProfilesUiState())
    private var reads = 0; private var captures = 0; private var imports = 0; private var clones = 0
    private var renames = 0; private var removes = 0; private var exports = 0; private var observations = 0; private var picked = 0; private var exportRetries = 0
    private var draftReloads = 0
    private fun ready() = SUMHProfilesUiState(
        loaded = true, capabilities = SUMHProfileCapabilities(SUMH_PROFILE_ACTIONS, known = true),
        listCurrent = true,
        items = listOf(item), bankRevision = revision, form = SUMHProfileForm("new-profile", "A new profile", "Keep this draft"),
    )
    private fun detail() = SUMHProfileResult("show", item.id, "complete", "inactive_profile", revision, profileRevision,
        document = SUMHProfileDocument("{\"name\":\"Before module update\"}\n", item.name, item.note,
            mapOf(SUMHProfileResource.Settings to 10, SUMHProfileResource.Modules to 2, SUMHProfileResource.UserHide to 3, SUMHProfileResource.Maps to 1)))
    private fun show(initial: SUMHProfilesUiState = ready(), dark: Boolean = false) {
        state.value = initial
        compose.setContent { MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
            Surface(Modifier.fillMaxSize()) {
                SUMHProfilesContent(state.value, SUMHProfilesActions(
                    refresh = { reads++ }, form = { state.value = state.value.copy(form = it) },
                    inspect = { state.value = state.value.copy(detail = detail()) },
                    resources = { state.value = state.value.copy(draft = state.value.draft.copy(resources = it), preview = null) },
                    chooseImport = { picked++ }, capture = { captures++ }, importProfile = { imports++ }, clone = { clones++ },
                    rename = { renames++ }, remove = { removes++ }, export = { exports++ }, observe = { observations++ },
                    retryExport = { exportRetries++ },
                    reloadDraft = { draftReloads++ },
                ), Modifier.fillMaxSize())
            }
        } }
    }
    private fun go(tag: String): SemanticsNodeInteraction {
        compose.onNodeWithTag("profiles_list").performScrollToNode(hasTestTag(tag))
        return compose.onNodeWithTag(tag)
    }

    @Test fun openingDoesNotReadOrMutateAndCapabilitiesAreRequired() {
        show(ready().copy(capabilities = SUMHProfileCapabilities(), items = emptyList(), bankRevision = null))
        assertEquals(0, reads); assertEquals(0, captures)
        go("profiles_capture").assertIsNotEnabled()
        go("profiles_refresh").assertIsEnabled().performClick(); assertEquals(1, reads)
    }

    @Test fun failedRefreshRetainsProfilesWithoutReportingEmptySuccess() {
        show(ready().copy(error = "bank_unreadable", listCurrent = false))
        go("profiles_error").assertTextContains("bank_unreadable", substring = true)
        go("profiles_open_profile-one").assertIsEnabled()
        compose.onAllNodesWithTag("profiles_empty").assertCountEquals(0)
        go("profiles_capture").assertIsNotEnabled()
    }

    @Test fun captureRequiresReviewAndCancelRetainsForm() {
        show()
        go("profiles_capture").performClick(); assertEquals(0, captures)
        compose.onNodeWithTag("profiles_confirm_cancel").performClick()
        assertEquals("Keep this draft", state.value.form.note)
        go("profiles_capture").performClick()
        compose.onNodeWithTag("profiles_confirm_capture").performClick(); assertEquals(1, captures)
    }

    @Test fun revisionChangeDisablesAnOpenDeleteConfirmation() {
        show(ready().copy(detail = detail()))
        go("profiles_remove").performClick()
        compose.runOnIdle { state.value = state.value.copy(bankRevision = "sha256-v1:" + "c".repeat(64)) }
        compose.onNodeWithTag("profiles_confirm_remove").assertIsNotEnabled()
        assertEquals(0, removes)
    }

    @Test fun cloneGetsIndependentIdAndRenameKeepsSelectedIdentity() {
        show(ready().copy(detail = detail()))
        go("profiles_clone").performClick()
        assertNotEquals(item.id, state.value.form.id)
        assertEquals(item.name, state.value.form.name)
        compose.onNodeWithTag("profiles_dialog_form_name").performScrollTo().performTextReplacement("Edited duplicate")
        assertEquals("Edited duplicate", state.value.form.name)
        capture("profiles-clone-edit", "profiles_confirmation")
        compose.onNodeWithTag("profiles_confirm_clone").performClick(); assertEquals(1, clones)
        go("profiles_rename").performClick()
        assertEquals(item.id, state.value.form.id)
        compose.onNodeWithTag("profiles_confirm_rename").performClick(); assertEquals(1, renames)
    }

    @Test fun pendingRequestExposesObservationAndNeverReplays() {
        val pending = SUMHProfilePending("remove", item.id, null, revision, profileRevision, "a".repeat(64), "dispatching")
        show(ready().copy(detail = detail(), pending = pending))
        assertEquals(0, observations); assertEquals(0, removes)
        go("profiles_remove").assertIsNotEnabled()
        go("profiles_acknowledge").assertIsNotEnabled()
        go("profiles_observe").performClick()
        assertEquals(1, observations); assertEquals(0, removes)
        go("profiles_export").assertIsEnabled()
    }

    @Test fun importNeedsPreflightAndReplacingDraftNeedsExplicitChoice() {
        show(ready().copy(draft = SUMHProfileDraft("{\"name\":\"Imported\"}"), draftFingerprint = "a".repeat(64)))
        go("profiles_import").assertIsNotEnabled()
        go("profiles_import_file").performClick(); assertEquals(0, picked)
        compose.onNodeWithTag("profiles_confirm_replace_import").performClick(); assertEquals(1, picked)
        compose.runOnIdle { state.value = state.value.copy(preview = SUMHProfileResult("import-preview", status = "complete", reason = "activation_not_implemented", diff = SUMHProfileDiff(emptyList(), "{}"))) }
        go("profiles_import").assertIsEnabled().performClick(); assertEquals(0, imports)
        compose.onNodeWithTag("profiles_confirm_import").performClick(); assertEquals(1, imports)
    }

    @Test fun resourceChoiceIsIndependentAndInvalidatesDisplayedPreview() {
        show(ready().copy(detail = detail(), preview = SUMHProfileResult("preview", status = "complete", diff = SUMHProfileDiff(emptyList(), "{}"))))
        go("profiles_resource_maps").assertIsOn().performClick().assertIsOff()
        assertFalse(SUMHProfileResource.Maps in state.value.draft.resources)
        assertNull(state.value.preview)
        assertEquals(0, captures); assertEquals(0, imports)
    }

    @Test fun hiddenIdentityConflictAndMapsEnabledChangeRemainVisible() {
        val diff = SUMHProfileDiff(listOf(
            SUMHProfileResourceDiff(SUMHProfileResource.UserHide, 0, 0, 1, 0, emptyList(), identityConflicts = listOf("same_id_different_path: /before → /after")),
            SUMHProfileResourceDiff(SUMHProfileResource.Maps, 0, 0, 0, 1, emptyList(), enabledChanges = listOf(SUMHProfileChange("enabled", "change", "false", "true"))),
        ), "{}")
        show(ready().copy(detail = detail(), preview = SUMHProfileResult("preview", status = "complete", diff = diff)))
        go("profiles_diff_scope").assertIsDisplayed()
        go("profiles_conflict_user_hide_0").assertIsDisplayed()
        go("profiles_change_maps_enabled_0").assertIsDisplayed().performClick()
        go("profiles_text_diff:maps:enabled:0:enabled:before_0").assertTextEquals("false")
        go("profiles_text_diff:maps:enabled:0:enabled:after_0").assertTextEquals("true")
    }

    @Test fun duplicateNamesStillOpenDistinctStableIds() {
        show(ready().copy(items = listOf(item, item.copy(id = "profile-two"))))
        go("profiles_open_profile-one").assertIsDisplayed()
        go("profiles_open_profile-two").assertIsDisplayed()
    }

    @Test fun missingTargetObservationCanBeAcknowledgedWithoutHistoricalSuccess() {
        val pending = SUMHProfilePending("remove", item.id, null, revision, profileRevision, "a".repeat(64), "dispatching")
        show(ready().copy(pending = pending, pendingObservation = SUMHProfileResult("observe", item.id, "missing", "current_target_missing", revision)))
        go("profiles_observation").assertTextContains("does not prove", substring = true)
        go("profiles_acknowledge").assertIsEnabled()
        assertEquals(0, removes)
    }

    @Test fun changedCaptureDraftInvalidatesOpenConfirmation() {
        show()
        go("profiles_capture").performClick()
        compose.runOnIdle { state.value = state.value.copy(form = state.value.form.copy(name = "Changed after review")) }
        compose.onNodeWithTag("profiles_confirm_capture").assertIsNotEnabled()
        assertEquals(0, captures)
    }

    @Test fun retainedLibraryStaysStaleAfterFormChangeAndErrorClears() {
        show(ready().copy(listCurrent = false, error = "bank_unreadable"))
        compose.runOnIdle { state.value = state.value.copy(error = null, form = state.value.form.copy(name = "Edited name")) }
        go("profiles_status").assertTextContains("previous read", substring = true)
        go("profiles_capture").assertIsNotEnabled()
        assertEquals(0, captures)
    }

    @Test fun staleEmptyLibraryDoesNotClaimNoSavedProfiles() {
        show(ready().copy(listCurrent = false, items = emptyList(), error = null))
        go("profiles_status").assertTextContains("previous read", substring = true)
        compose.onAllNodesWithTag("profiles_empty").assertCountEquals(0)
        go("profiles_capture").assertIsNotEnabled()
    }

    @Test fun restoredFrozenExportCanRetryWithoutDetailOrBackendCapabilities() {
        val artifact = SUMHProfileExport("frozen-token", "{\"name\":\"Frozen\"}", "older-profile", profileRevision)
        show(SUMHProfilesUiState(loaded = true, exportArtifact = artifact))
        assertEquals(0, reads); assertEquals(0, exportRetries); assertEquals(0, exports)
        go("profiles_frozen_export").assertExists()
        compose.onNodeWithText("older-profile").assertExists()
        compose.onNodeWithText(profileRevision).assertExists()
        go("profiles_retry_export").assertIsEnabled().performClick()
        assertEquals(1, exportRetries); assertEquals(0, reads); assertEquals(0, exports)
    }

    @Test fun frozenExportIdentityStaysSeparateFromSelectedProfile() {
        val artifact = SUMHProfileExport("frozen-token", "{}", "older-profile", revision)
        show(ready().copy(detail = detail(), exportArtifact = artifact))
        go("profiles_frozen_export").assertExists()
        compose.onNodeWithText("older-profile").assertExists()
        compose.onNodeWithText(revision).assertExists()
        go("profiles_export").assertIsEnabled()
        assertEquals(0, exports); assertEquals(0, exportRetries)
    }

    @Test fun importedDraftFingerprintChangeInvalidatesOpenConfirmation() {
        show(ready().copy(draft = SUMHProfileDraft("{}"), draftFingerprint = "a".repeat(64),
            preview = SUMHProfileResult("import-preview", status = "complete", diff = SUMHProfileDiff(emptyList(), "{}"))))
        go("profiles_import").assertIsEnabled().performClick()
        compose.runOnIdle { state.value = state.value.copy(draftFingerprint = "b".repeat(64)) }
        compose.onNodeWithTag("profiles_confirm_import").assertIsNotEnabled()
        assertEquals(0, imports)
    }

    @Test fun draftRecoveryRequiresExplicitChoiceAndDoesNotTriggerBackendRead() {
        show(ready().copy(draftNeedsReload = true, draft = SUMHProfileDraft("retained")))
        assertEquals(0, draftReloads); assertEquals(0, reads)
        go("profiles_import_file").assertIsNotEnabled()
        go("profiles_import_discard").assertIsNotEnabled()
        go("profiles_resource_maps").assertIsNotEnabled()
        go("profiles_reload_draft").performClick(); assertEquals(0, draftReloads)
        compose.onNodeWithTag("profiles_cancel_reload_draft").performClick()
        assertEquals("retained", state.value.draft.document)
        go("profiles_reload_draft").performClick()
        compose.onNodeWithTag("profiles_confirm_reload_draft").performClick()
        assertEquals(1, draftReloads); assertEquals(0, reads); assertEquals(0, imports)
    }

    @Test @Config(qualifiers = "zh-rCN-w320dp-h760dp-xhdpi")
    fun narrowChineseLargeTextKeepsLibraryAndCaptureReachable() {
        RuntimeEnvironment.setFontScale(2f)
        show()
        go("profiles_open_profile-one").assertIsDisplayed(); capture("profiles-library-narrow-large-text")
        go("profiles_capture").assertIsDisplayed().assertIsEnabled(); capture("profiles-capture-narrow-large-text")
    }

    @Test @Config(qualifiers = "zh-rCN-w760dp-h360dp-land-xhdpi")
    fun darkLandscapeLargeTextKeepsDetailAndDeleteConfirmationReachable() {
        RuntimeEnvironment.setFontScale(2f)
        show(ready().copy(detail = detail()), dark = true)
        go("profiles_export").assertIsDisplayed().assertIsEnabled()
        go("profiles_remove").assertIsDisplayed().performClick()
        compose.onNodeWithTag("profiles_confirm_remove").assertIsDisplayed().assertIsEnabled()
        capture("profiles-delete-landscape-dark", "profiles_confirmation")
    }

    private fun capture(name: String, tag: String? = null) {
        val output = File("build/reports/sumh-layout/$name.png"); output.parentFile?.mkdirs()
        output.outputStream().use { (if (tag == null) compose.onRoot() else compose.onNodeWithTag(tag)).captureToImage().asAndroidBitmap().compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
