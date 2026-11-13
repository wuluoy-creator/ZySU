package com.zying.zysu.ui.viewmodel

import android.app.Application
import android.content.Context
import android.os.Looper
import android.system.Os
import android.system.StructUtsname
import androidx.lifecycle.ViewModelStore
import com.zying.zysu.ui.screen.home.HomeLayoutStyle
import com.zying.zysu.ui.screen.home.HomeFactField
import com.zying.zysu.ui.screen.home.HomeFactVisibility
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.annotation.Implements
import org.robolectric.annotation.Implementation
import kotlin.test.assertFalse
import kotlin.test.assertTrue
import kotlin.test.assertEquals

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], shadows = [HomePreferenceOsShadow::class])
class HomeDisplayPreferencesTest {
    @Test fun obsoleteModePreferencesDoNotChangeRetainedHomeOrTriggerRefresh() {
        val context = RuntimeEnvironment.getApplication()
        val preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        preferences.edit().clear().putBoolean("is_simple_mode", true)
            .putBoolean("is_kernel_simple_mode", true).commit()
        val model = HomeViewModel()
        val store = ViewModelStore().apply { put("home", model) }
        model.loadUserSettings(context)
        val expected = HomeFactField.entries.map { it.id }.toSet()
        assertEquals(expected, model.visibleHomeFactIds)
        assertFalse(model.isHideLinkCard)
        assertFalse(model.isHideVersion)
        val refreshKey = model.dataRefreshTrigger.value
        preferences.edit().putBoolean("is_simple_mode", false).putBoolean("is_kernel_simple_mode", false).commit()
        shadowOf(Looper.getMainLooper()).idle()
        assertEquals(expected, model.visibleHomeFactIds)
        assertFalse(model.isCoreDataLoaded)
        assertFalse(model.isRefreshing)
        assertEquals(refreshKey, model.dataRefreshTrigger.value)
        store.clear()
    }

    @Test fun homeReflectsDisplayChangesAndReleasesItsListener() {
        val context = RuntimeEnvironment.getApplication()
        val preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        val model = HomeViewModel()
        val store = ViewModelStore().apply { put("home", model) }
        model.loadUserSettings(context)
        assertFalse(model.showDeviceDetails)
        assertEquals(HomeLayoutStyle.Circle, model.homeLayout)
        assertTrue("device" in model.visibleHomeFactIds)

        preferences.edit().putBoolean("home_show_device_details", true).putString("home_layout", "grid")
            .putBoolean("is_hide_version", true).commit()
        shadowOf(Looper.getMainLooper()).idle()
        assertTrue(model.showDeviceDetails)
        assertEquals(HomeLayoutStyle.Grid, model.homeLayout)
        assertTrue(model.isHideVersion)
        HomeFactVisibility.setVisible(preferences, HomeFactField.Device, false)
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse("device" in model.visibleHomeFactIds)

        store.clear()
        preferences.edit().putBoolean("home_show_device_details", false).putString("home_layout", "focus").commit()
        shadowOf(Looper.getMainLooper()).idle()
        assertTrue(model.showDeviceDetails)
        assertEquals(HomeLayoutStyle.Grid, model.homeLayout)
        HomeFactVisibility.setVisible(preferences, HomeFactField.Device, true)
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse("device" in model.visibleHomeFactIds)
    }
}

/** Home's initial kernel snapshot is independent of the preference behavior under test. */
@Implements(Os::class)
class HomePreferenceOsShadow {
    companion object {
        @JvmStatic
        @Implementation
        fun uname() = StructUtsname("Linux", "localhost", "6.1.0", "test", "aarch64")
    }
}
