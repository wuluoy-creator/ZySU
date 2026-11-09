package com.zying.zysu.ui.webui;

import android.app.Activity;
import android.graphics.Bitmap;
import android.net.http.SslError;
import android.os.Message;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.webkit.ClientCertRequest;
import android.webkit.HttpAuthHandler;
import android.webkit.RenderProcessGoneDetail;
import android.webkit.SafeBrowsingResponse;
import android.webkit.SslErrorHandler;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

/** Download-script fallback for system WebViews without document-start scripts. */
@SuppressWarnings("deprecation")
final class DownloadClient extends WebViewClient {
    private final WebViewClient delegate;
    private final String script;

    DownloadClient(WebViewClient delegate, String script) {
        this.delegate = delegate;
        this.script = script;
    }

    @Override public void onPageStarted(WebView view, String url, Bitmap icon) {
        delegate.onPageStarted(view, url, icon);
    }

    @Override public void onPageFinished(WebView view, String url) {
        delegate.onPageFinished(view, url);
        view.evaluateJavascript(script, null);
    }

    @Override public void doUpdateVisitedHistory(WebView view, String url, boolean reload) {
        delegate.doUpdateVisitedHistory(view, url, reload);
        view.evaluateJavascript(script, null);
    }

    @Override public void onLoadResource(WebView view, String url) {
        delegate.onLoadResource(view, url);
    }

    @Override public void onPageCommitVisible(WebView view, String url) {
        delegate.onPageCommitVisible(view, url);
    }

    @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
        return delegate.shouldOverrideUrlLoading(view, request);
    }

    @Override public boolean shouldOverrideUrlLoading(WebView view, String url) {
        return delegate.shouldOverrideUrlLoading(view, url);
    }

    @Nullable @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
        return delegate.shouldInterceptRequest(view, request);
    }

    @Nullable @Override public WebResourceResponse shouldInterceptRequest(WebView view, String url) {
        return delegate.shouldInterceptRequest(view, url);
    }

    @Override public void onTooManyRedirects(WebView view, Message cancel, Message proceed) {
        delegate.onTooManyRedirects(view, cancel, proceed);
    }

    @Override public void onReceivedError(WebView view, int code, String description, String url) {
        delegate.onReceivedError(view, code, description, url);
    }

    @Override public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
        delegate.onReceivedError(view, request, error);
    }

    @Override public void onReceivedHttpError(WebView view, WebResourceRequest request, WebResourceResponse response) {
        delegate.onReceivedHttpError(view, request, response);
    }

    @Override public void onFormResubmission(WebView view, Message cancel, Message resend) {
        delegate.onFormResubmission(view, cancel, resend);
    }

    @Override public void onReceivedSslError(WebView view, SslErrorHandler handler, SslError error) {
        delegate.onReceivedSslError(view, handler, error);
    }

    @Override public void onReceivedClientCertRequest(WebView view, ClientCertRequest request) {
        delegate.onReceivedClientCertRequest(view, request);
    }

    @Override public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler, String host, String realm) {
        delegate.onReceivedHttpAuthRequest(view, handler, host, realm);
    }

    @Override public boolean shouldOverrideKeyEvent(WebView view, KeyEvent event) {
        return delegate.shouldOverrideKeyEvent(view, event);
    }

    @Override public void onUnhandledKeyEvent(WebView view, KeyEvent event) {
        delegate.onUnhandledKeyEvent(view, event);
    }

    @Override public void onScaleChanged(WebView view, float oldScale, float newScale) {
        delegate.onScaleChanged(view, oldScale, newScale);
    }

    @Override public void onReceivedLoginRequest(WebView view, String realm, String account, String args) {
        delegate.onReceivedLoginRequest(view, realm, account, args);
    }

    @Override public boolean onRenderProcessGone(@NonNull WebView view, @NonNull RenderProcessGoneDetail detail) {
        if (!delegate.onRenderProcessGone(view, detail)) {
            if (view.getParent() instanceof ViewGroup parent) parent.removeView(view);
            view.destroy();
            if (view.getContext() instanceof Activity activity) activity.finish();
        }
        return true;
    }

    @Override public void onSafeBrowsingHit(WebView view, WebResourceRequest request, int threatType, SafeBrowsingResponse callback) {
        delegate.onSafeBrowsingHit(view, request, threatType, callback);
    }
}
