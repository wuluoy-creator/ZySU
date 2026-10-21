package com.zying.zysu.ui.sumh.util

import com.google.gson.JsonArray
import org.junit.Test
import kotlin.test.*

class SUMHProfilesTest {
    @Test fun fullHideCapacityRetainsUnicodeAndNewlines() {
        val document = ProfileFixtures.document(4096)
        assertEquals(4096, document.resourceCounts[SUMHProfileResource.UserHide])
        assertContains(document.text, "\\n ")
        val result = SUMHProfilesParser.result(profileUtf8(ProfileFixtures.show(hidden = 4096).toString()), ProfileFixtures.showRequest(hidden = 4096))
        assertTrue(result.complete); assertEquals(document, result.document)
    }
    @Test fun profileRejectsUnknownSchemaFieldsAndDuplicateKeys() {
        val text = ProfileFixtures.document().text
        assertFails { SUMHProfilesParser.document(text.replace("\"schema_version\":1", "\"schema_version\":2")) }
        assertFails { SUMHProfilesParser.document(text.replaceFirst("{", "{\"extra\":1,")) }
        assertFails { SUMHProfilesParser.document(text.replaceFirst("\"name\":", "\"name\":\"duplicate\", \"name\":")) }
        assertFails { SUMHProfilesParser.document(text + "{}") }
        assertFails { SUMHProfilesParser.document("\uFEFF$text") }
    }
    @Test fun invalidUtf8AndMalformedSurrogatesAreNotReplaced() {
        assertFails { profileDecode(byteArrayOf(0xc0.toByte(), 0x80.toByte())) }
        assertFails { profileUtf8("\uD800") }
        assertFails { SUMHProfilesParser.document(ProfileFixtures.document().text.replace("配置", "\\ud800")) }
    }
    @Test fun duplicateResourceIdentitiesAreRejected() {
        val value = ProfileFixtures.profile(2); val rows = value.getAsJsonObject("resources").getAsJsonObject("user_hide").getAsJsonArray("rules")
        rows[1].asJsonObject.addProperty("id", rows[0].asJsonObject.get("id").asString)
        assertFails { SUMHProfilesParser.document(value.toString()) }
    }
    @Test fun scopeSelectionAndRequestKindsAreExact() {
        val request = SUMHProfileRequest("import-preview", document = ProfileFixtures.document().text, resources = setOf(SUMHProfileResource.Maps))
        assertContains(profileDecode(SUMHProfilesParser.request(request)), "\"resources\":[\"maps\"]")
        assertFails { SUMHProfilesParser.request(request.copy(resources = emptySet())) }
        assertFails { SUMHProfilesParser.request(request.copy(id = "unexpected")) }
        assertFails { SUMHProfilesParser.request(request.copy(action = "activate")) }
    }
    @Test fun revisionBindingRejectsWrongIdBankAndDocument() {
        val request = ProfileFixtures.showRequest()
        assertFails { SUMHProfilesParser.result(profileUtf8(ProfileFixtures.show().toString()), request.copy(id = "other")) }
        assertFails { SUMHProfilesParser.result(profileUtf8(ProfileFixtures.show().toString()), request.copy(bankRevision = "sha256-v1:" + "b".repeat(64))) }
        val changed = ProfileFixtures.show(); changed.getAsJsonObject("profile").addProperty("name", "changed")
        assertFails { SUMHProfilesParser.result(profileUtf8(changed.toString()), request) }
    }
    @Test fun inactiveAndRuntimeClaimsAreEnforced() {
        for (field in listOf("applied", "profile_active", "active_configuration_saved", "can_activate", "runtime_evaluated", "global_atomic")) {
            val value = ProfileFixtures.show().apply { addProperty(field, true) }
            assertFails(field) { SUMHProfilesParser.result(profileUtf8(value.toString()), ProfileFixtures.showRequest()) }
        }
    }
    @Test fun unchangedDoesNotClaimFileDurability() {
        val value = SUMHProfilesParser.result(profileUtf8(ProfileFixtures.mutation("rename", "unchanged").toString()), ProfileFixtures.showRequest("rename"))
        assertTrue(value.saved); assertTrue(value.readbackConfirmed); assertFalse(value.durabilityConfirmed)
    }
    @Test fun captureAliasAndRemoveNullRevisionAreAccepted() {
        val captured = SUMHProfilesParser.result(profileUtf8(ProfileFixtures.mutation("capture").toString()), SUMHProfileRequest("capture", "example", ProfileFixtures.bank))
        assertTrue(captured.complete)
        val removed = SUMHProfilesParser.result(profileUtf8(ProfileFixtures.mutation("remove").toString()), ProfileFixtures.showRequest("remove"))
        assertTrue(removed.complete); assertNull(removed.profileRevision)
        assertFails { SUMHProfilesParser.result(profileUtf8(ProfileFixtures.mutation("import").toString()), SUMHProfileRequest("capture", "example", ProfileFixtures.bank)) }
    }
    @Test fun namespaceFailureAfterSavedIsUnknown() {
        val receipt = ProfileFixtures.mutation("capture").apply { addProperty("namespace_restore_failed", true); addProperty("namespace_restore_errno", 5) }
        val value = SUMHProfilesParser.result(profileUtf8(receipt.toString()), SUMHProfileRequest("capture", "example", ProfileFixtures.bank))
        assertTrue(value.unknown); assertFalse(value.complete); assertTrue(value.saved)
    }
    @Test fun mapsEnabledDiffAndCountValidation() {
        val request = SUMHProfileRequest("import-preview", document = ProfileFixtures.document().text, resources = setOf(SUMHProfileResource.Maps))
        val value = ProfileFixtures.preview()
        assertEquals("true", SUMHProfilesParser.result(profileUtf8(value.toString()), request).diff!!.resources.single().enabledChanges.single().after)
        value.getAsJsonObject("diff").getAsJsonObject("resources").getAsJsonObject("maps").getAsJsonObject("counts").addProperty("added", 1)
        assertFails { SUMHProfilesParser.result(profileUtf8(value.toString()), request) }
    }
    @Test fun mismatchedOrDuplicatedScopesAreRejected() {
        val request = SUMHProfileRequest("import-preview", resources = setOf(SUMHProfileResource.Maps))
        val value = ProfileFixtures.preview().apply { add("selected_resources", JsonArray().apply { add("maps"); add("maps") }) }
        assertFails { SUMHProfilesParser.result(profileUtf8(value.toString()), request) }
        assertFails { SUMHProfilesParser.result(profileUtf8(ProfileFixtures.preview().toString()), request.copy(resources = setOf(SUMHProfileResource.Settings))) }
    }
    @Test fun transferReceiptRequiresCompleteAndExactBoundedShape() {
        val valid = """{"schema_version":1,"kind":"sumh.profile.transfer","complete":true,"errno":0,"bytes":12}"""
        assertEquals(12, SUMHProfilesParser.transfer(profileUtf8(valid)))
        assertFails { SUMHProfilesParser.transfer(profileUtf8(valid.replace("true", "false"))) }
        assertFails { SUMHProfilesParser.transfer(profileUtf8(valid.replace("\"errno\":0", "\"errno\":5"))) }
        assertFails { SUMHProfilesParser.transfer(profileUtf8(valid.replace("\"bytes\":12", "\"bytes\":25165825"))) }
    }
    @Test fun capacityAndExactInputMetadata() {
        assertNull(SUMHProfileForm("valid-id", "中".repeat(42), "note ").error)
        assertNotNull(SUMHProfileForm("valid-id", "中".repeat(43), "note ").error)
        assertNotNull(SUMHProfileForm(" id", "name").error)
        assertFails { SUMHProfilesParser.document(" ".repeat(SUMH_PROFILE_BYTES + 1)) }
    }
}
