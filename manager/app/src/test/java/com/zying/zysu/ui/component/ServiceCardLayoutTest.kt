package com.zying.zysu.ui.component

import android.app.Application
import android.graphics.Bitmap
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asAndroidBitmap
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.test.*
import androidx.compose.ui.test.junit4.v2.createAndroidComposeRule
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.dp
import com.zying.zysu.R
import com.zying.zysu.integrity.KsudIntegrityStatus
import com.zying.zysu.ui.sumh.SUMHStatusSummaryCard
import com.zying.zysu.ui.sumh.util.SUMHUiAdapter
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35], qualifiers = "en-w320dp-h900dp-mdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class ServiceCardLayoutTest {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()
    private val longVersion = "v3.6.2-release-2026-build-with-extra-runtime-diagnostics"
    private val longProtocol = "13-runtime-protocol-with-extra-driver-diagnostics"
    private val system = SUMHUiAdapter.SystemInfo("6.12.0", "/dev/sumh", emptyList(), emptyList(), false, null,
        viewsEnabled = true)

    @Test fun narrowPairSharesHeaderDetailAndStatusBaselines() {
        show {
            Row(Modifier.fillMaxWidth().height(IntrinsicSize.Min), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                KsudFixture(Modifier.weight(1f).fillMaxHeight().testTag("ksud-card"))
                SumhFixture(Modifier.weight(1f).fillMaxHeight().testTag("sumh-card"))
            }
        }
        listOf("header", "value", "status").forEach { row ->
            val ksud = compose.onNodeWithTag("service-ksud-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
            val sumh = compose.onNodeWithTag("service-sumh-$row", useUnmergedTree = true).getUnclippedBoundsInRoot()
            assertEquals(ksud.top.value, sumh.top.value, 1f)
            assertEquals(ksud.bottom.value, sumh.bottom.value, 1f)
        }
        listOf("ksud", "sumh").forEach { service ->
            compose.onNodeWithTag("service-$service-value").assertHeightIsAtLeast(48.dp)
            val card = compose.onNodeWithTag("$service-card").getUnclippedBoundsInRoot()
            val header = compose.onNodeWithTag("service-$service-header", useUnmergedTree = true).getUnclippedBoundsInRoot()
            val footer = compose.onNodeWithTag("service-$service-status", useUnmergedTree = true).getUnclippedBoundsInRoot()
            assertEquals(16f, header.top.value - card.top.value, 1f)
            assertEquals(16f, card.bottom.value - footer.bottom.value, 1f)
        }
        capture("paired-320")
    }

    @Test fun largeFontFullWidthCardsGrowWithoutTruncatingLabelsOrStatus() {
        show(fontScale = 2f, dark = true) {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                KsudFixture(Modifier.fillMaxWidth().testTag("ksud-card"))
                SumhFixture(Modifier.fillMaxWidth().testTag("sumh-card"))
            }
        }
        listOf("ksud", "sumh").forEach { service ->
            val card = compose.onNodeWithTag("$service-card").fetchSemanticsNode().boundsInRoot
            listOf("header", "value", "status").forEach { row ->
                val child = compose.onNodeWithTag("service-$service-$row", useUnmergedTree = true).fetchSemanticsNode().boundsInRoot
                assertTrue(child.left >= card.left && child.right <= card.right && child.bottom <= card.bottom)
            }
            compose.onNodeWithTag("service-$service-value").assertHeightIsAtLeast(48.dp)
        }
        compose.onNodeWithText(compose.activity.getString(R.string.service_card_protocol)).assertIsDisplayed()
        compose.onNodeWithText(compose.activity.getString(R.string.home_sumh_views_on)).assertIsDisplayed()
        capture("stacked-large-dark")
    }

    @Test fun serviceActionsRemainIndependentFromDetails() {
        var syncs = 0
        var refreshes = 0
        show {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                KsudFixture(Modifier.fillMaxWidth().testTag("ksud-card"), sync = { syncs++ })
                SumhFixture(Modifier.fillMaxWidth().testTag("sumh-card"), refresh = { refreshes++ })
            }
        }
        compose.onNodeWithTag("service-ksud-header", useUnmergedTree = true).performTouchInput { click() }
        compose.onNodeWithTag("service-sumh-header", useUnmergedTree = true).performTouchInput { click() }
        assertEquals(1, syncs)
        assertEquals(1, refreshes)
        compose.onNodeWithText(longVersion, substring = true).assertDoesNotExist()
        compose.onNodeWithTag("service-ksud-value").performClick()
        compose.onNodeWithText(longVersion, substring = true).assertIsDisplayed()
        assertEquals(1, syncs)
        compose.onNodeWithText(compose.activity.getString(android.R.string.ok)).performClick()
        compose.onNodeWithTag("service-sumh-value").performClick()
        compose.onNodeWithText(longProtocol, substring = true).assertIsDisplayed()
        assertEquals(1, refreshes)
    }

    @Test fun loadingDisablesMutationsButKeepsVersionDetailsReadable() {
        show {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                KsudFixture(Modifier.fillMaxWidth().testTag("ksud-card"), loading = true)
                SumhFixture(Modifier.fillMaxWidth().testTag("sumh-card"), loading = true)
            }
        }
        compose.onNodeWithTag("ksud-card").assertIsNotEnabled()
        compose.onNodeWithTag("sumh-card").assertIsNotEnabled()
        compose.onNodeWithTag("service-sumh-value").assertIsEnabled().performClick()
        compose.onNodeWithText(longProtocol, substring = true).assertIsDisplayed()
    }

    @Test fun protocolMismatchKeepsFullSelectableReasonBehindDetails() {
        val diagnostic = "Kernel protocol mismatch: expected 13, received 12; runtime details remain available."
        show {
            SUMHStatusSummaryCard(SUMHUiAdapter.SUMHStatus.AVAILABLE, longProtocol,
                system.copy(sumhMismatch = true, mismatchMessage = diagnostic), {}, modifier = Modifier.fillMaxWidth())
        }
        compose.onNodeWithText(diagnostic, substring = true).assertDoesNotExist()
        compose.onNodeWithText(compose.activity.getString(R.string.home_status_protocol_mismatch)).assertIsDisplayed()
        compose.onNodeWithTag("service-sumh-value").performClick()
        compose.onNodeWithText(diagnostic, substring = true).assertIsDisplayed()
    }

    @Test fun dedicatedStatusScreenRetainsRuntimeFields() {
        show {
            SUMHStatusSummaryCard(SUMHUiAdapter.SUMHStatus.AVAILABLE, "13", system, {},
                modifier = Modifier.fillMaxWidth(), includeRuntimeDetails = true, includeKernel = true)
        }
        compose.onNodeWithText(system.kernel).assertIsDisplayed()
        compose.onNodeWithText(system.mountBase).assertIsDisplayed()
        capture("runtime-details")
    }

    @Composable
    private fun KsudFixture(modifier: Modifier, loading: Boolean = false, sync: () -> Unit = {}) {
        KsudStatusCard(longVersion, longVersion, KsudIntegrityStatus.MATCH, loading, false, null, null, sync, modifier)
    }

    @Composable
    private fun SumhFixture(modifier: Modifier, loading: Boolean = false, refresh: () -> Unit = {}) {
        SUMHStatusSummaryCard(SUMHUiAdapter.SUMHStatus.AVAILABLE, longProtocol, system, refresh, modifier, loading)
    }

    private fun show(fontScale: Float = 1f, dark: Boolean = false, content: @Composable () -> Unit) {
        compose.setContent {
            val density = LocalDensity.current
            CompositionLocalProvider(LocalDensity provides Density(density.density, fontScale)) {
                MaterialTheme(colorScheme = if (dark) darkColorScheme() else lightColorScheme()) {
                    Surface(Modifier.fillMaxSize()) { Column(Modifier.fillMaxWidth().padding(16.dp)) { content() } }
                }
            }
        }
    }

    private fun capture(name: String) {
        val file = File("../../out/service-card-revision-ui/$name.png")
        file.parentFile?.mkdirs()
        val bitmap = compose.onRoot().captureToImage().asAndroidBitmap()
        file.outputStream().use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
    }
}
