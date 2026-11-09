package com.zying.zysu.ui.webui

import android.webkit.WebView
import androidx.core.net.toUri
import androidx.webkit.ScriptHandler
import androidx.webkit.WebViewCompat
import androidx.webkit.WebViewFeature
import org.json.JSONObject

/** Preserve each host's navigation, asset loading, dialogs and renderer handling. */
internal class WebUIDownloadSupport(webView: WebView, download: WebUIDownloadInterface) {
    private val scriptHandler: ScriptHandler?

    init {
        val script = webView.context.assets.open("webview/download.js").bufferedReader().use { it.readText() }
        webView.addJavascriptInterface(download, "ksu_download")
        webView.setDownloadListener { url, agent, disposition, mime, _ ->
            val uri = url.toUri()
            if (uri.scheme == "blob" || uri.scheme == "data" ||
                (uri.host != null && uri.host == webView.url?.toUri()?.host)) {
                val name = android.webkit.URLUtil.guessFileName(url, disposition, mime)
                webView.evaluateJavascript("$script\nwindow.__ksu_download_url(${JSONObject.quote(url)}, ${JSONObject.quote(name)});", null)
            } else {
                download.enqueueDownload(url, agent, disposition, mime)
            }
        }
        scriptHandler = if (WebViewFeature.isFeatureSupported(WebViewFeature.DOCUMENT_START_SCRIPT)) {
            WebViewCompat.addDocumentStartJavaScript(webView, script, setOf("*"))
        } else {
            // Older system WebViews do not support document-start scripts.
            webView.webViewClient = DownloadClient(webView.webViewClient, script)
            null
        }
    }

    fun destroy() { scriptHandler?.remove() }
}
