package com.zying.zysu.ui.screen.home

import android.app.Application
import android.content.Context
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class HomeFactVisibilityTest {
    private val preferences get() = RuntimeEnvironment.getApplication().getSharedPreferences("settings", Context.MODE_PRIVATE)

    @Before fun clearPreferences() { preferences.edit().clear().commit() }

    @Test fun registryCoversEveryStableCoreAndBuildFieldWithReadableLabels() {
        val expected = setOf("device", "kernel", "selinux", "manager", "android", "mode", "seccomp", "zygisk",
            "mount", "security", "architecture", "build", "fingerprint")
        assertEquals(expected, HomeFactField.entries.map { it.id }.toSet())
        assertEquals(expected.size, HomeFactField.entries.size)
        assertEquals(expected, HomeFactVisibility.read(preferences))
        HomeFactField.entries.forEach { assertTrue(RuntimeEnvironment.getApplication().getString(it.label).isNotBlank()) }
    }

    @Test fun legacyPerFieldHiddenFlagsPreserveExistingChoices() {
        preferences.edit().putBoolean("is_hide_seccomp_status", true).putBoolean("is_hide_zygisk_Implement", true)
            .putBoolean("is_hide_meta_module_Implement", true).commit()
        val visible = HomeFactVisibility.read(preferences)
        assertFalse("seccomp" in visible)
        assertFalse("zygisk" in visible)
        assertFalse("mount" in visible)
        assertTrue("android" in visible)
        assertTrue("fingerprint" in visible)
    }

    @Test fun fieldChoicesShareTheLegacyHiddenState() {
        HomeFactVisibility.setVisible(preferences, HomeFactField.Android, true)
        HomeFactVisibility.setVisible(preferences, HomeFactField.Zygisk, true)
        assertTrue("android" in HomeFactVisibility.read(preferences))
        assertTrue("zygisk" in HomeFactVisibility.read(preferences))
        assertFalse(preferences.getBoolean("is_hide_zygisk_Implement", true))
        preferences.edit().putBoolean("is_hide_zygisk_Implement", true).commit()
        assertFalse("zygisk" in HomeFactVisibility.read(preferences))
        preferences.edit().putBoolean("is_hide_zygisk_Implement", false).commit()
        assertTrue("zygisk" in HomeFactVisibility.read(preferences))
        HomeFactVisibility.setVisible(preferences, HomeFactField.Mount, false)
        assertTrue(preferences.getBoolean("is_hide_meta_module_Implement", false))
        HomeFactVisibility.setVisible(preferences, HomeFactField.Seccomp, false)
        assertTrue(preferences.getBoolean("is_hide_seccomp_status", false))
    }

    @Test fun removedModePreferencesDoNotHideAnyFieldsOrOverrideChoices() {
        preferences.edit().putBoolean("is_simple_mode", true).putBoolean("is_kernel_simple_mode", true).commit()
        assertEquals(HomeFactField.entries.map { it.id }.toSet(), HomeFactVisibility.read(preferences))
        assertFalse(HomeFactVisibility.observes("is_simple_mode"))
        assertFalse(HomeFactVisibility.observes("is_kernel_simple_mode"))
        HomeFactVisibility.setVisible(preferences, HomeFactField.Android, false)
        HomeFactVisibility.setVisible(preferences, HomeFactField.Zygisk, false)
        val selected = HomeFactVisibility.read(preferences)
        preferences.edit().putBoolean("is_simple_mode", false).putBoolean("is_kernel_simple_mode", false).commit()
        assertEquals(selected, HomeFactVisibility.read(preferences))
    }

    @Test fun everyFieldPersistsIndependentlyAndFiltersWithoutChangingOrderOrData() {
        val facts = HomeFactField.entries.map { HomeFact(it.id, "Label ${it.id}", "Complete value ${it.id}") }
        HomeFactField.entries.forEach { field ->
            HomeFactVisibility.setVisible(preferences, field, false)
            val restored = RuntimeEnvironment.getApplication().getSharedPreferences("settings", Context.MODE_PRIVATE)
            assertEquals(facts.filterNot { it.id == field.id }, visibleHomeFacts(facts, HomeFactVisibility.read(restored)))
            HomeFactVisibility.setVisible(preferences, field, true)
        }
        HomeFactField.entries.forEach { HomeFactVisibility.setVisible(preferences, it, false) }
        assertTrue(visibleHomeFacts(facts, HomeFactVisibility.read(preferences)).isEmpty())
    }
}
