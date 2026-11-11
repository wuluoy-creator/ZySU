package com.zying.zysu.ui.viewmodel

import android.app.Application
import android.content.Context
import android.os.Looper
import android.system.Os
import android.system.StructUtsname
import androidx.lifecycle.ViewModelStore
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

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], shadows = [HomePreferenceOsShadow::class])
class HomeDisplayPreferencesTest {
    @Test fun homeReflectsDisplayChangesAndReleasesItsListener() {
        val context = RuntimeEnvironment.getApplication()
        val preferences = context.getSharedPreferences("settings", Context.MODE_PRIVATE)
        preferences.edit().clear().commit()
        val model = HomeViewModel()
        val store = ViewModelStore().apply { put("home", model) }
        model.loadUserSettings(context)
        assertTrue(model.showDeviceMetrics)
        assertTrue(model.showCounts)
        assertFalse(model.showDeviceDetails)
        assertFalse(model.useListLayout)

        preferences.edit().putBoolean("home_show_metrics", false).putBoolean("home_show_counts", false)
            .putBoolean("home_show_device_details", true).putString("home_layout", "list")
            .putBoolean("is_hide_version", true).commit()
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse(model.showDeviceMetrics)
        assertFalse(model.showCounts)
        assertTrue(model.showDeviceDetails)
        assertTrue(model.useListLayout)
        assertTrue(model.isHideVersion)

        store.clear()
        preferences.edit().putBoolean("home_show_metrics", true).commit()
        shadowOf(Looper.getMainLooper()).idle()
        assertFalse(model.showDeviceMetrics)
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
