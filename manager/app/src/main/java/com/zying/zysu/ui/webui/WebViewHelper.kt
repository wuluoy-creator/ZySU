package com.zying.zysu.ui.webui

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.webkit.JavascriptInterface
import android.webkit.JsPromptResult
import android.webkit.JsResult
import android.webkit.ValueCallback
import android.webkit.WebChromeClient
import android.webkit.WebView
import androidx.webkit.WebViewAssetLoader
import com.zying.zysu.BuildConfig
import com.zying.zysu.R
import com.zying.zysu.ui.util.createRootShell
import com.zying.zysu.ui.util.listModules
import com.zying.zysu.ui.util.setTaskDescriptionLabel
import com.zying.zysu.ui.viewmodel.SuperUserViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File

// Javascript bridge calls run on WebView's worker thread. The shared loader joins
// an active scan and reuses the service snapshot, including all Android users.
internal fun webUIApps(): List<SuperUserViewModel.AppInfo> = runBlocking {
    SuperUserViewModel.loadAppsForWebUI()
}

@SuppressLint("SetJavaScriptEnabled")
internal suspend fun prepareWebView(activity: Activity, moduleId: String, state: WebUIState) {
    withContext(Dispatchers.IO) {
        val modules = JSONArray(listModules())
        val module = (0 until modules.length()).asSequence()
            .map { modules.getJSONObject(it) }
            .find { it.optString("dir_id", it.optString("id")) == moduleId }

        if (module == null) {
            withContext(Dispatchers.Main) {
                state.uiEvent = WebUIEvent.Error(activity.getString(R.string.no_such_module, moduleId))
            }
            return@withContext
        }

        val name = module.optString("name", moduleId)
        if (!module.optBoolean("web") || !module.optBoolean("enabled") ||
            module.optBoolean("update") || module.optBoolean("remove")
        ) {
            withContext(Dispatchers.Main) {
                state.uiEvent = WebUIEvent.Error(activity.getString(R.string.module_unavailable, name))
            }
            return@withContext
        }

        val shell = createRootShell(true)
        var attached = false
        try {
            withContext(Dispatchers.Main) {
                state.rootShell = shell
                attached = true
                state.moduleName = name
                state.modDir = "/data/adb/modules/$moduleId"
                activity.setTaskDescriptionLabel("${activity.getString(R.string.app_name)} - $name")

                val prefs = activity.getSharedPreferences("settings", Context.MODE_PRIVATE)
                WebView.setWebContentsDebuggingEnabled(
                    BuildConfig.DEBUG && prefs.getBoolean("enable_web_debugging", false)
                )

                val webView = WebView(activity)
                state.webView = webView
                webView.setBackgroundColor(Color.TRANSPARENT)
                webView.settings.apply {
                    javaScriptEnabled = true
                    domStorageEnabled = true
                    allowFileAccess = false
                }

                val assetLoader = WebViewAssetLoader.Builder()
                    .setDomain("mui.kernelsu.org")
                    .addPathHandler(
                        "/",
                        SuFilePathHandler(
                            File("${state.modDir}/webroot"), shell,
                            { state.currentInsets },
                            { webView.post { state.isInsetsEnabled = true } },
                            { state.colorsCss },
                        )
                    )
                    .build()

                webView.webViewClient = ModuleWebViewClient(
                    activity, assetLoader,
                    { view ->
                        if (state.webView === view) state.webView = null
                        state.requestExit()
                    },
                    { view ->
                        state.webCanGoBack = view.canGoBack()
                        if (state.isInsetsEnabled) view.evaluateJavascript(state.currentInsets.js, null)
                    },
                )
                webView.webChromeClient = object : WebChromeClient() {
                    override fun onJsAlert(view: WebView?, url: String?, message: String?, result: JsResult?): Boolean {
                        if (message == null || result == null) return false
                        state.uiEvent = WebUIEvent.ShowAlert(message, result)
                        return true
                    }

                    override fun onJsConfirm(view: WebView?, url: String?, message: String?, result: JsResult?): Boolean {
                        if (message == null || result == null) return false
                        state.uiEvent = WebUIEvent.ShowConfirm(message, result)
                        return true
                    }

                    override fun onJsPrompt(
                        view: WebView?, url: String?, message: String?, defaultValue: String?, result: JsPromptResult?
                    ): Boolean {
                        if (message == null || result == null) return false
                        state.uiEvent = WebUIEvent.ShowPrompt(message, defaultValue.orEmpty(), result)
                        return true
                    }

                    override fun onShowFileChooser(
                        webView: WebView?, callback: ValueCallback<Array<Uri>>?, params: FileChooserParams?
                    ): Boolean {
                        state.filePathCallback?.onReceiveValue(null)
                        state.filePathCallback = callback
                        val intent = params?.createIntent() ?: Intent(Intent.ACTION_GET_CONTENT).apply { type = "*/*" }
                        if (params?.mode == FileChooserParams.MODE_OPEN_MULTIPLE) {
                            intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                        }
                        state.uiEvent = WebUIEvent.ShowFileChooser(intent)
                        return true
                    }
                }

                val bridge = KsuWebViewInterface(activity, webView, moduleId, state)
                state.webViewInterface = bridge
                webView.addJavascriptInterface(bridge, "ksu")
                bridge.attachDownloads()
                state.uiEvent = WebUIEvent.WebViewReady
            }
        } finally {
            if (!attached) shell.runCatching { close() }
        }
    }
}

internal class KsuWebViewInterface(
    activity: Activity,
    webView: WebView,
    moduleId: String,
    private val state: WebUIState,
) : WebViewInterface(activity, webView, moduleId) {
    @JavascriptInterface
    fun enableEdgeToEdge(enable: Boolean) {
        webView.post { state.isInsetsEnabled = enable }
    }

    @JavascriptInterface
    override fun fullScreen(enable: Boolean) {
        super.fullScreen(enable)
        enableEdgeToEdge(enable)
    }
}
