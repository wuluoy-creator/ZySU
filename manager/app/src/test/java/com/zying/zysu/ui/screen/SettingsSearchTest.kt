package com.zying.zysu.ui.screen

import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertNull
import kotlin.test.assertTrue

class SettingsSearchTest {
    @Test fun matchesLocalizedFeaturesAndAllQueryWords() {
        assertNotNull(settingsSearchScore("字体 圆角", "外观", "颜色和壁纸", "字体大小 卡片圆角"))
        assertNull(settingsSearchScore("字体 授权", "外观", "颜色", "字体大小"))
        assertNull(settingsSearchScore("   ", "外观", "", ""))
    }

    @Test fun foldsAccentsCaseAndWhitespace() {
        assertEquals(settingsSearchScore("ÉCRAN", "Écran", "", ""), settingsSearchScore(" ecran ", "Écran", "", ""))
        assertNotNull(settingsSearchScore("dark\n\tmode", "Appearance", "Dark mode", ""))
    }

    @Test fun titleMatchesRankAheadOfFeatureMatches() {
        val title = settingsSearchScore("display", "Display", "", "")!!
        val feature = settingsSearchScore("display", "Appearance", "", "Display size")!!
        assertTrue(title > feature)
    }
}
