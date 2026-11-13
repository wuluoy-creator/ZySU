package ui.screen.moreSettings

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.util.Log
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.*
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.Description
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.getValue
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import com.zying.zysu.R
import com.zying.zysu.BuildConfig
import com.zying.zysu.ui.component.KsuIsValid
import com.zying.zysu.ui.component.LocalBottomBarPadding
import com.zying.zysu.ui.screen.WorkspaceTabs
import com.zying.zysu.ui.component.YukiIcon
import com.zying.zysu.ui.component.YukiIconBadge
import com.zying.zysu.ui.component.YukiTopAppBar
import com.zying.zysu.ui.component.YukiTopBarTitle
import com.zying.zysu.ui.theme.*
import com.yalantis.ucrop.UCrop
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ui.screen.moreSettings.component.ColorCircle
import ui.screen.moreSettings.component.AmoledSetting
import ui.screen.moreSettings.component.AppearancePreview
import ui.screen.moreSettings.component.BackgroundBlurSetting
import ui.screen.moreSettings.component.HomeDisplaySettings
import ui.screen.moreSettings.component.NavigationStyleSetting
import ui.screen.moreSettings.component.TypographyAndShapeSettings
import ui.screen.moreSettings.component.LanguageSelectionDialog
import ui.screen.moreSettings.component.MoreSettingsDialogs
import ui.screen.moreSettings.component.MoreSettingsItemPosition
import ui.screen.moreSettings.component.SettingItem
import ui.screen.moreSettings.component.SettingsCard
import ui.screen.moreSettings.component.SettingsControlGroup
import ui.screen.moreSettings.component.SwitchSettingItem
import ui.screen.moreSettings.state.MoreSettingsState
import ui.screen.moreSettings.util.LocaleHelper
import kotlin.math.roundToInt

enum class PreferenceCategory(val titleRes: Int) {
    Appearance(R.string.settings_category_appearance),
    Display(R.string.settings_category_display),
    Advanced(R.string.settings_category_security),
    WebUI(R.string.settings_category_webui),
}

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun MoreSettingsScreen(navigator: DestinationsNavigator) {
    var selectedCategory by rememberSaveable { mutableStateOf(PreferenceCategory.Appearance) }
    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior()
    Scaffold(
        topBar = { MoreSettingsTopBar(onBack = { navigator.popBackStack() }, scrollBehavior = scrollBehavior) },
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        Column(Modifier.fillMaxSize().padding(padding).consumeWindowInsets(padding)) {
            WorkspaceTabs(
                labels = PreferenceCategory.entries.map { stringResource(it.titleRes) },
                selected = selectedCategory.ordinal,
                onSelected = { selectedCategory = PreferenceCategory.entries[it] },
            )
            MoreSettingsContent(selectedCategory)
        }
    }
}

@SuppressLint("LocalContextConfigurationRead", "LocalContextResourcesRead", "ObsoleteSdkInt")
@Composable
fun MoreSettingsContent(
    category: PreferenceCategory,
    scrollable: Boolean = true,
    contentPadding: PaddingValues = PaddingValues(0.dp),
) {
    val context = LocalContext.current
    val coroutineScope = rememberCoroutineScope()
    val prefs = remember { context.getSharedPreferences("settings", Context.MODE_PRIVATE) }
    val systemIsDark = isSystemInDarkTheme()

    val settingsState = remember { MoreSettingsState(context, prefs, systemIsDark) }
    val settingsHandlers = remember { MoreSettingsHandlers(context, prefs, settingsState) }
    var settingsLoaded by remember { mutableStateOf(false) }
    val cropToolbarColor = MaterialTheme.colorScheme.surfaceContainerLow
    val cropToolbarWidgetColor = MaterialTheme.colorScheme.onSurface
    val cropAccentColor = MaterialTheme.colorScheme.primary
    val cropToolbarTitle = stringResource(R.string.settings_custom_background)

    val cropImageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        when (result.resultCode) {
            Activity.RESULT_OK -> {
                val croppedUri = result.data?.let(UCrop::getOutput)
                if (croppedUri != null) {
                    settingsHandlers.handleCustomBackground(croppedUri)
                } else {
                    Log.e("WallpaperCropper", "Crop completed without an output Uri")
                    Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
                }
            }

            UCrop.RESULT_ERROR -> {
                val error = result.data?.let(UCrop::getError)
                Log.e("WallpaperCropper", "Failed to crop custom background", error)
                Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
            }
        }
    }

    val pickImageLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.GetContent()
    ) { uri ->
        uri?.let {
            try {
                cropImageLauncher.launch(
                    createWallpaperCropIntent(
                        context = context,
                        sourceUri = it,
                        toolbarTitle = cropToolbarTitle,
                        style = WallpaperCropStyle(
                            toolbarColor = cropToolbarColor.toArgb(),
                            toolbarWidgetColor = cropToolbarWidgetColor.toArgb(),
                            accentColor = cropAccentColor.toArgb(),
                            lightStatusBar = cropToolbarColor.luminance() > 0.5f,
                        ),
                    )
                )
            } catch (error: Exception) {
                Log.e("WallpaperCropper", "Failed to launch custom background cropper", error)
                Toast.makeText(context, R.string.operation_failed, Toast.LENGTH_SHORT).show()
            }
        }
    }

    LaunchedEffect(Unit) {
        settingsHandlers.initializeSettings()
        settingsLoaded = true
    }

    // Native and theme values must be loaded before an editable control is shown.
    if (!settingsLoaded) {
        Box(
            if (scrollable) Modifier.fillMaxSize().padding(contentPadding) else Modifier.fillMaxWidth().padding(16.dp),
            contentAlignment = Alignment.Center,
        ) {
            CircularProgressIndicator()
        }
        return
    }

    MoreSettingsDialogs(
        state = settingsState,
        handlers = settingsHandlers
    )

    val categoryScrollState = key(category) { rememberScrollState() }
    Column(
        modifier = (if (scrollable) {
            Modifier.fillMaxSize().verticalScroll(categoryScrollState)
        } else {
            Modifier.fillMaxWidth()
        }).padding(contentPadding)
            .padding(horizontal = 18.dp, vertical = if (scrollable) 8.dp else 0.dp)
            .padding(bottom = if (scrollable) LocalBottomBarPadding.current else 0.dp),
    ) {
        when (category) {
            PreferenceCategory.Appearance -> AppearanceSettings(
                state = settingsState,
                handlers = settingsHandlers,
                pickImageLauncher = pickImageLauncher,
                coroutineScope = coroutineScope,
            )
            PreferenceCategory.Display -> CustomizationSettings(settingsState, settingsHandlers)
            PreferenceCategory.Advanced -> KsuIsValid { AdvancedSettings(settingsState, settingsHandlers) }
            PreferenceCategory.WebUI -> KsuIsValid { WebUIDebugSettings(settingsState, settingsHandlers) }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MoreSettingsTopBar(
    onBack: () -> Unit,
    scrollBehavior: TopAppBarScrollBehavior
) {
    val title: @Composable () -> Unit = {
        YukiTopBarTitle(text = stringResource(R.string.more_settings))
    }
    val navigationIcon: @Composable () -> Unit = {
        IconButton(onClick = onBack) {
            YukiIcon(
                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                contentDescription = stringResource(R.string.back)
            )
        }
    }
    YukiTopAppBar(
        title = title,
        navigationIcon = navigationIcon,
        scrollBehavior = scrollBehavior,
    )
}

@Composable
private fun AppearanceSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    pickImageLauncher: ActivityResultLauncher<String>,
    coroutineScope: CoroutineScope
) {
    AppearancePreview()
    SettingsCard(title = stringResource(R.string.appearance_settings), icon = Icons.Filled.Translate) {
        LanguageSetting(state = state)

        SwitchSettingItem(
            icon = Icons.AutoMirrored.Filled.ArrowBack,
            title = stringResource(R.string.predictive_back_gesture),
            summary = stringResource(R.string.predictive_back_gesture_summary),
            checked = state.predictiveBackEnabled,
            groupPosition = MoreSettingsItemPosition.Last,
            onChange = handlers::handlePredictiveBackChange
        )
    }
    SettingsCard(title = stringResource(R.string.theme_mode), icon = Icons.Filled.Palette) {
        SettingItem(
            icon = Icons.Default.DarkMode,
            title = stringResource(R.string.theme_mode),
            subtitle = state.themeOptions[state.themeMode],
            onClick = { state.showThemeModeDialog = true }
        )


        SwitchSettingItem(
            icon = Icons.Filled.ColorLens,
            title = stringResource(R.string.dynamic_color_title),
            summary = stringResource(R.string.dynamic_color_summary),
            checked = state.useDynamicColor,
            groupPosition = MoreSettingsItemPosition.Middle,
            onChange = handlers::handleDynamicColorChange
        )


        AnimatedVisibility(
            visible = !state.useDynamicColor,
            enter = fadeIn() + expandVertically(),
            exit = fadeOut() + shrinkVertically()
        ) {
            ThemeColorSelection(state = state)
        }
        AmoledSetting()
    }
    TypographyAndShapeSettings()
    NavigationStyleSetting()
    SettingsCard(title = stringResource(R.string.app_dpi_title), icon = Icons.Filled.FormatSize) {
        DpiSettings(state = state, handlers = handlers)
    }
    SettingsCard(title = stringResource(R.string.settings_custom_background), icon = Icons.Filled.Wallpaper) {
        CustomBackgroundSettings(
            state = state,
            handlers = handlers,
            pickImageLauncher = pickImageLauncher,
            coroutineScope = coroutineScope
        )
    }
}

@Composable
private fun CustomizationSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsCard(title = stringResource(R.string.appearance_display_modules), icon = Icons.Filled.Extension) {

        SwitchSettingItem(
            icon = Icons.Filled.Info,
            title = stringResource(R.string.show_more_module_info),
            summary = stringResource(R.string.show_more_module_info_summary),
            checked = state.showMoreModuleInfo,
            groupPosition = MoreSettingsItemPosition.First,
            onChange = handlers::handleShowMoreModuleInfoChange
        )

        ModuleDescriptionLinesSetting(state, handlers)
        SwitchSettingItem(
            icon = Icons.Filled.VisibilityOff,
            title = stringResource(R.string.hide_tag_card),
            summary = stringResource(R.string.hide_tag_card_summary),
            checked = state.isHideTagRow,
            groupPosition = MoreSettingsItemPosition.Last,
            onChange = handlers::handleHideTagRowChange
        )
    }
    SettingsCard(title = stringResource(R.string.appearance_display_home), icon = Icons.Filled.Home) {
        HomeDisplaySettings()
        SwitchSettingItem(
            icon = Icons.Filled.VisibilityOff,
            title = stringResource(R.string.hide_kernel_kernelsu_version),
            summary = stringResource(R.string.hide_kernel_kernelsu_version_summary),
            checked = state.isHideVersion,
            onChange = handlers::handleHideVersionChange
        )
        SwitchSettingItem(
            icon = Icons.Filled.VisibilityOff,
            title = stringResource(R.string.hide_link_card),
            summary = stringResource(R.string.hide_link_card_summary),
            checked = state.isHideLinkCard,
            groupPosition = MoreSettingsItemPosition.Last,
            onChange = handlers::handleHideLinkCardChange
        )
    }
    SettingsCard(title = stringResource(R.string.appearance_display_visibility), icon = Icons.Filled.Visibility) {
        SwitchSettingItem(
            icon = Icons.Filled.VisibilityOff,
            title = stringResource(R.string.hide_other_info),
            summary = stringResource(R.string.hide_other_info_summary),
            checked = state.isHideOtherInfo,
            groupPosition = MoreSettingsItemPosition.Only,
            onChange = handlers::handleHideOtherInfoChange
        )
    }
}

@Composable
private fun ModuleDescriptionLinesSetting(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsControlGroup {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            YukiIconBadge(Icons.Outlined.Description)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = stringResource(R.string.settings_module_description_max_lines),
                    style = MaterialTheme.typography.titleSmall
                )
                Text(
                    text = stringResource(R.string.settings_module_description_max_lines_summary),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Text(
                text = "${state.moduleDescriptionMaxLines} " + stringResource(R.string.unit_lines),
                style = MaterialTheme.typography.labelMedium
            )
        }
        Slider(
            value = state.moduleDescriptionMaxLines.toFloat(),
            onValueChange = { handlers.handleModuleDescriptionMaxLinesChange(it.toInt()) },
            valueRange = 1f..5f,
            steps = 3
        )
    }
}

@Composable
private fun AdvancedSettings(state: MoreSettingsState, handlers: MoreSettingsHandlers) {
    SettingsCard {
        SwitchSettingItem(
            icon = Icons.Filled.Security,
            title = stringResource(R.string.selinux),
            summary = if (state.selinuxEnabled) stringResource(R.string.selinux_enabled) else stringResource(R.string.selinux_disabled),
            checked = state.selinuxEnabled,
            groupPosition = MoreSettingsItemPosition.Only,
            onChange = handlers::handleSelinuxChange,
        )
    }
}

@Composable
private fun WebUIDebugSettings(state: MoreSettingsState, handlers: MoreSettingsHandlers) {
    // Release APKs do not contain the Eruda console; keep the existing build guard.
    if (BuildConfig.DEBUG) {
        SettingsCard(title = stringResource(R.string.settings_category_webui)) {
            SwitchSettingItem(
                icon = Icons.Filled.DeveloperMode,
                title = stringResource(R.string.enable_web_debugging),
                summary = stringResource(R.string.enable_web_debugging_summary),
                checked = state.enableWebDebugging,
                groupPosition = if (state.enableWebDebugging && state.webuiEngine == "wx") {
                    MoreSettingsItemPosition.First
                } else {
                    MoreSettingsItemPosition.Only
                },
                onChange = handlers::handleWebDebuggingChange,
            )
            AnimatedVisibility(
                visible = state.enableWebDebugging && state.webuiEngine == "wx",
                enter = fadeIn() + expandVertically(),
                exit = fadeOut() + shrinkVertically(),
            ) {
                SwitchSettingItem(
                    icon = Icons.Filled.FormatListNumbered,
                    title = stringResource(R.string.use_webuix_eruda),
                    summary = stringResource(R.string.use_webuix_eruda_summary),
                    checked = state.useWebUIXEruda,
                    groupPosition = MoreSettingsItemPosition.Last,
                    onChange = handlers::handleWebUIXErudaChange,
                )
            }
        }
    }
}

@Composable
private fun ThemeColorSelection(state: MoreSettingsState) {
    SettingItem(
        icon = Icons.Default.Palette,
        title = stringResource(R.string.theme_color),
        subtitle = when (ThemeConfig.currentTheme) {
            is ThemeColors.Green -> stringResource(R.string.color_green)
            is ThemeColors.Purple -> stringResource(R.string.color_purple)
            is ThemeColors.Orange -> stringResource(R.string.color_orange)
            is ThemeColors.Pink -> stringResource(R.string.color_pink)
            is ThemeColors.Gray -> stringResource(R.string.color_gray)
            is ThemeColors.Yellow -> stringResource(R.string.color_yellow)
            is ThemeColors.TransPride -> stringResource(R.string.color_trans_pride)  // 🏳️‍⚧️
            else -> stringResource(R.string.color_default)
        },
        groupPosition = MoreSettingsItemPosition.Middle,
        onClick = { state.showThemeColorDialog = true },
        trailingContent = {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(start = 8.dp)
            ) {
                val theme = ThemeConfig.currentTheme
                val isDark = ThemeConfig.forceDarkMode ?: isSystemInDarkTheme()

                ColorCircle(
                    color = if (isDark) theme.primaryDark else theme.primaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
                ColorCircle(
                    color = if (isDark) theme.secondaryDark else theme.secondaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
                ColorCircle(
                    color = if (isDark) theme.tertiaryDark else theme.tertiaryLight,
                    isSelected = false,
                    modifier = Modifier.padding(horizontal = 2.dp)
                )
            }
        }
    )
}

@Composable
private fun DpiSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsControlGroup {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text(
                text = stringResource(R.string.app_dpi_summary),
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Surface(
                shape = RoundedCornerShape(percent = 50),
                color = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
            ) {
                Text(
                    text = handlers.getDpiFriendlyName(state.tempDpi),
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                    style = MaterialTheme.typography.labelLarge,
                )
            }
        }
    }


    DpiSliderControls(state = state, handlers = handlers)
}

@Composable
private fun DpiSliderControls(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers
) {
    SettingsControlGroup {
        val sliderValue by animateFloatAsState(
            targetValue = state.tempDpi.toFloat(),
            label = "DPI Slider Animation"
        )

        Slider(
            value = sliderValue,
            onValueChange = { newValue ->
                state.tempDpi = newValue.toInt()
                state.isDpiCustom = !state.dpiPresets.containsValue(state.tempDpi)
            },
            valueRange = 160f..600f,
            steps = 11,
            colors = SliderDefaults.colors(
                thumbColor = MaterialTheme.colorScheme.primary,
                activeTrackColor = MaterialTheme.colorScheme.primary,
                inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
            )
        )

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .selectableGroup()
                .padding(top = 8.dp),
        ) {
            state.dpiPresets.forEach { (name, dpi) ->
                val isSelected = state.tempDpi == dpi
                val buttonColor = if (isSelected)
                    MaterialTheme.colorScheme.primaryContainer
                else
                    MaterialTheme.colorScheme.surfaceVariant

                Box(
                    modifier = Modifier
                        .weight(1f)
                        .padding(horizontal = 2.dp)
                        .heightIn(min = 48.dp)
                        .clip(RoundedCornerShape(percent = 50))
                        .background(buttonColor)
                        .selectable(selected = isSelected, role = Role.RadioButton) {
                            state.tempDpi = dpi
                            state.isDpiCustom = false
                        }
                        .padding(vertical = 8.dp, horizontal = 4.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = name,
                        style = MaterialTheme.typography.labelMedium,
                        color = if (isSelected)
                            MaterialTheme.colorScheme.onPrimaryContainer
                        else
                            MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }

        Text(
            text = if (state.isDpiCustom)
                "${stringResource(R.string.dpi_size_custom)}: ${state.tempDpi}"
            else
                "${handlers.getDpiFriendlyName(state.tempDpi)}: ${state.tempDpi}",
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.padding(top = 8.dp)
        )

        Button(
            onClick = { state.showDpiConfirmDialog = true },
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 8.dp),
            enabled = state.tempDpi != state.currentDpi
        ) {
            YukiIcon(
                Icons.Default.Check,
                contentDescription = null,
                modifier = Modifier.size(16.dp)
            )
            Spacer(modifier = Modifier.width(8.dp))
            Text(stringResource(R.string.dpi_apply_settings))
        }
    }
}

@Composable
private fun CustomBackgroundSettings(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    pickImageLauncher: ActivityResultLauncher<String>,
    coroutineScope: CoroutineScope
) {
    val hasCustomBackground = ThemeConfig.customBackgroundUri != null
    SwitchSettingItem(
        icon = Icons.Filled.Wallpaper,
        title = stringResource(id = R.string.settings_custom_background),
        summary = stringResource(id = R.string.settings_custom_background_summary),
        checked = state.isCustomBackgroundEnabled,
        groupPosition = if (hasCustomBackground) {
            MoreSettingsItemPosition.Middle
        } else {
            MoreSettingsItemPosition.Last
        },
        onChange = { isChecked ->
            if (isChecked) {
                pickImageLauncher.launch("image/*")
            } else {
                handlers.handleRemoveCustomBackground()
            }
        }
    )

    AnimatedVisibility(
        visible = hasCustomBackground,
        enter = fadeIn() + slideInVertically(),
        exit = fadeOut() + slideOutVertically()
    ) {
        Column {
            SettingItem(
                icon = Icons.Filled.Image,
                title = stringResource(R.string.settings_change_background),
                subtitle = stringResource(R.string.settings_change_background_summary),
                groupPosition = MoreSettingsItemPosition.Middle,
                onClick = { pickImageLauncher.launch("image/*") }
            )

            BackgroundAdjustmentControls(
                state = state,
                handlers = handlers,
                coroutineScope = coroutineScope
            )
        }
    }
}

@Composable
private fun BackgroundAdjustmentControls(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    SettingsControlGroup(groupPosition = MoreSettingsItemPosition.Last) {
        AlphaSlider(state = state, handlers = handlers, coroutineScope = coroutineScope)

        DimSlider(state = state, handlers = handlers, coroutineScope = coroutineScope)
    }
    BackgroundBlurSetting()
}

@Composable
private fun AlphaSlider(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(bottom = 4.dp)
    ) {
        YukiIcon(
            Icons.Filled.Opacity,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = stringResource(R.string.settings_card_alpha),
            style = MaterialTheme.typography.titleSmall
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "${(state.cardAlpha * 100).roundToInt()}%",
            style = MaterialTheme.typography.labelMedium,
        )
    }

    val alphaSliderValue by animateFloatAsState(
        targetValue = state.cardAlpha,
        label = "Alpha Slider Animation"
    )

    Slider(
        value = alphaSliderValue,
        onValueChange = { newValue ->
            handlers.handleCardAlphaChange(newValue)
        },
        onValueChangeFinished = {
            coroutineScope.launch(start = CoroutineStart.UNDISPATCHED) {
                withContext(NonCancellable + Dispatchers.IO) {
                    saveCardConfig(handlers.context)
                }
            }
        },
        valueRange = 0f..1f,
        steps = 20,
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

@Composable
private fun DimSlider(
    state: MoreSettingsState,
    handlers: MoreSettingsHandlers,
    coroutineScope: CoroutineScope
) {
    Row(
        verticalAlignment = Alignment.CenterVertically,
        modifier = Modifier.padding(top = 16.dp, bottom = 4.dp)
    ) {
        YukiIcon(
            Icons.Filled.LightMode,
            contentDescription = null,
            modifier = Modifier.size(20.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(modifier = Modifier.width(8.dp))
        Text(
            text = stringResource(R.string.settings_card_dim),
            style = MaterialTheme.typography.titleSmall
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = "${(state.cardDim * 100).roundToInt()}%",
            style = MaterialTheme.typography.labelMedium,
        )
    }

    val dimSliderValue by animateFloatAsState(
        targetValue = state.cardDim,
        label = "Dim Slider Animation"
    )

    Slider(
        value = dimSliderValue,
        onValueChange = { newValue ->
            handlers.handleCardDimChange(newValue)
        },
        onValueChangeFinished = {
            coroutineScope.launch(start = CoroutineStart.UNDISPATCHED) {
                withContext(NonCancellable + Dispatchers.IO) {
                    saveCardConfig(handlers.context)
                }
            }
        },
        valueRange = 0f..1f,
        steps = 20,
        colors = SliderDefaults.colors(
            thumbColor = MaterialTheme.colorScheme.primary,
            activeTrackColor = MaterialTheme.colorScheme.primary,
            inactiveTrackColor = MaterialTheme.colorScheme.surfaceVariant
        )
    )
}

fun saveCardConfig(context: Context) {
    CardConfig.save(context)
}

@Composable
private fun LanguageSetting(state: MoreSettingsState) {
    val context = LocalContext.current
    val resources = LocalResources.current
    val language = stringResource(id = R.string.settings_language)

    // Compute display name based on current app locale
    val currentLanguageDisplay = remember(state.currentAppLocale, resources.configuration) {
        val locale = state.currentAppLocale
        if (locale != null) {
            locale.getDisplayName(locale)
        } else {
            resources.getString(R.string.language_system_default)
        }
    }

    SettingItem(
        icon = Icons.Filled.Translate,
        title = language,
        subtitle = currentLanguageDisplay,
        groupPosition = MoreSettingsItemPosition.First,
        onClick = { state.showLanguageDialog = true }
    )

    // Language Selection Dialog
    if (state.showLanguageDialog) {
        LanguageSelectionDialog(
            onLanguageSelected = { newLocale ->
                // Update local state immediately
                state.currentAppLocale = LocaleHelper.getCurrentAppLocale(context)
                // Apply locale change immediately for Android < 13
                LocaleHelper.restartActivity(context)
            },
            onDismiss = { state.showLanguageDialog = false }
        )
    }
}
