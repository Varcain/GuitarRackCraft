/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

package com.varcain.guitarrackcraft.ui.modgui

import android.annotation.SuppressLint
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import kotlinx.coroutines.delay
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.webkit.WebViewAssetLoader
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.RackManager
import java.io.File

/**
 * JavaScript bridge for modgui: the host exposes setParameter(symbol, value) and getParameter(symbol).
 */
class ModguiHostBridge(
    @Volatile var pluginIndex: Int,
    pluginInfo: PluginInfo
) {
    private val symbolToIndex: Map<String, Int> = pluginInfo.ports
        .filter { it.isControl && !it.isAudio }
        .associate { it.symbol to it.index }

    @JavascriptInterface
    fun setParameter(symbol: String, value: Float) {
        symbolToIndex[symbol]?.let { portIndex ->
            RackManager.setParameter(pluginIndex, portIndex, value)
        }
    }

    @JavascriptInterface
    fun getParameter(symbol: String): Float {
        return symbolToIndex[symbol]?.let { portIndex ->
            RackManager.getParameter(pluginIndex, portIndex)
        } ?: 0f
    }

    var onContentSize: ((Int, Int) -> Unit)? = null
        internal set
    var onFilePickerRequested: (() -> Unit)? = null
        internal set
    var onModelSelected: ((String) -> Unit)? = null
        internal set
    var webView: WebView? = null
        internal set

    @JavascriptInterface
    fun setContentSize(width: Int, height: Int) {
        onContentSize?.invoke(width, height)
    }

    /** Set from JS thread, read from UI thread touch listener — volatile for cross-thread visibility. */
    @Volatile
    var isKnobDragging = false

    @JavascriptInterface
    fun setKnobActive(active: Boolean) {
        isKnobDragging = active
        webView?.post {
            webView?.parent?.requestDisallowInterceptTouchEvent(active)
        }
    }

    @JavascriptInterface
    fun requestFilePicker() {
        webView?.post {
            onFilePickerRequested?.invoke()
        }
    }

    @JavascriptInterface
    fun selectModelPath(path: String) {
        webView?.post {
            onModelSelected?.invoke(path)
        }
    }
}

@SuppressLint("SetJavaScriptEnabled")
@Composable
fun ModguiScreen(
    pluginIndex: Int,
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current
    val pluginInfo = remember(pluginIndex) { RackManager.getRackPluginInfo(pluginIndex) }

    if (pluginInfo == null || !pluginInfo.hasModgui) {
        Box(modifier = Modifier.fillMaxSize()) {
            Text(
                text = if (pluginInfo == null) "Plugin not found" else "No modgui UI for this plugin",
                modifier = Modifier.padding(16.dp)
            )
        }
        return
    }

    val basePath = pluginInfo.modguiBasePath
    val iconTemplate = pluginInfo.modguiIconTemplate
    val baseDir = File(basePath)
    val templateData = remember(basePath, pluginInfo) {
        parseModguiTtlAndPluginInfo(basePath, pluginInfo)
    }
    val assetLoader = remember(context, basePath, iconTemplate, templateData) {
        buildAssetLoader(context, baseDir, iconTemplate, templateData, pluginInfo, inline = false)
    }
    val loadUrl = "https://modgui.app/$iconTemplate"

    val bridge = remember {
        ModguiHostBridge(pluginIndex, pluginInfo)
    }
    bridge.pluginIndex = pluginIndex  // keep current after reorders

    Box(modifier = Modifier.fillMaxSize()) {
        AndroidView(
            factory = {
                buildModguiWebView(context, assetLoader, bridge, loadUrl).apply {
                    setScrollBarStyle(WebView.SCROLLBARS_OUTSIDE_OVERLAY)
                    clipChildren = false
                    clipToPadding = false
                    isFocusable = true
                }
            },
            modifier = Modifier.fillMaxSize()
        )
        // Floating back button
        IconButton(
            onClick = onNavigateBack,
            modifier = Modifier
                .align(Alignment.TopStart)
                .padding(8.dp)
        ) {
            Icon(
                Icons.Default.ArrowBack,
                contentDescription = "Back",
                tint = Color.White
            )
        }
    }
}

/**
 * Inline modgui WebView composable for embedding in the rack. Scaled to fit screen width.
 * The WebView renders at the content's natural CSS pixel size, then WebView's built-in
 * overview mode scales it to fit the view width. Height is set proportionally.
 */
@SuppressLint("SetJavaScriptEnabled", "ClickableViewAccessibility")
@Composable
fun InlineModguiView(
    pluginIndex: Int,
    pluginInfo: PluginInfo,
    isVisible: Boolean = true,
    modifier: Modifier = Modifier,
    onContentSize: ((Int, Int) -> Unit)? = null,
    onFilePickerRequested: (() -> Unit)? = null,
    modelDisplayName: String? = null,
    modelFiles: List<File> = emptyList(),
    onModelSelected: ((String) -> Unit)? = null
) {
    val context = LocalContext.current
    val basePath = pluginInfo.modguiBasePath
    val iconTemplate = pluginInfo.modguiIconTemplate
    val baseDir = File(basePath)
    val templateData = remember(basePath, pluginInfo) {
        parseModguiTtlAndPluginInfo(basePath, pluginInfo)
    }
    val assetLoader = remember(context, basePath, iconTemplate, templateData) {
        buildAssetLoader(context, baseDir, iconTemplate, templateData, pluginInfo, inline = true)
    }
    val loadUrl = "https://modgui.app/$iconTemplate"
    val bridge = remember {
        ModguiHostBridge(pluginIndex, pluginInfo)
    }
    bridge.pluginIndex = pluginIndex  // keep current after reorders

    // Content natural size in CSS pixels, reported by JS after load
    var contentWidth by remember { mutableStateOf(0) }
    var contentHeight by remember { mutableStateOf(0) }

    bridge.onContentSize = { w, h ->
        contentWidth = w
        contentHeight = h
        onContentSize?.invoke(w, h)
    }
    bridge.onFilePickerRequested = onFilePickerRequested
    bridge.onModelSelected = onModelSelected

    // Push model display name into the modgui WebView when it changes
    LaunchedEffect(modelDisplayName) {
        if (modelDisplayName != null) {
            val escaped = modelDisplayName.replace("\\", "\\\\").replace("'", "\\'")
            bridge.webView?.post {
                bridge.webView?.evaluateJavascript(
                    "if(typeof _modSetPathDisplay==='function')_modSetPathDisplay('$escaped');",
                    null
                )
            }
        }
    }

    // Push model file list into the modgui WebView when it changes
    LaunchedEffect(modelFiles) {
        if (modelFiles.isNotEmpty()) {
            val json = modelFiles.joinToString(",", "[", "]") { f ->
                val name = f.nameWithoutExtension.replace("\\", "\\\\").replace("\"", "\\\"")
                val path = f.absolutePath.replace("\\", "\\\\").replace("\"", "\\\"")
                """{"name":"$name","path":"$path"}"""
            }
            val escaped = json.replace("'", "\\'")
            bridge.webView?.post {
                bridge.webView?.evaluateJavascript(
                    "if(typeof _modSetModelList==='function')_modSetModelList('$escaped');",
                    null
                )
            }
        }
    }

    // Delay WebView creation to avoid race with X11/EGL teardown.
    // WebView initialization triggers EGL driver cleanup that can destroy
    // mutexes shared with HWUI threads, causing "pthread_mutex_lock on
    // destroyed mutex" crash when switching from X11 to MODGUI.
    var isWebViewReady by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        delay(1000) // 1 second delay for X11 cleanup to complete
        isWebViewReady = true
    }

    // Periodically refresh modgui port visuals from native values so changes
    // made via X11 UI or sliders are reflected in the modgui knobs/toggles.
    // Only runs when visible to save CPU when scrolled off-screen.
    LaunchedEffect(bridge, isVisible) {
        if (!isVisible) return@LaunchedEffect
        while (true) {
            delay(200) // 5Hz refresh rate
            bridge.webView?.post {
                bridge.webView?.evaluateJavascript(
                    "if(typeof _modRefreshPorts==='function')_modRefreshPorts();", null
                )
            }
        }
    }

    BoxWithConstraints(modifier = modifier.fillMaxWidth()) {
        val containerWidthPx = constraints.maxWidth
        val containerHeightPx = constraints.maxHeight

        if (!isWebViewReady) {
            // Show placeholder while waiting for X11 cleanup
            Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                CircularProgressIndicator()
            }
            return@BoxWithConstraints
        }

        AndroidView(
            factory = {
                buildModguiWebView(context, assetLoader, bridge, loadUrl).also {
                    bridge.webView = it
                }
            },
            update = { webView ->
                // Scale content to fill container
                if (contentWidth > 0 && contentHeight > 0) {
                    webView.evaluateJavascript(
                        "if (typeof scaleToFill === 'function') scaleToFill($containerWidthPx, $containerHeightPx);",
                        null
                    )
                }
            },
            modifier = Modifier
                .fillMaxWidth()
                .fillMaxHeight()
        )
    }
}

@SuppressLint("SetJavaScriptEnabled")
private fun buildModguiWebView(
    context: android.content.Context,
    assetLoader: WebViewAssetLoader,
    bridge: ModguiHostBridge,
    loadUrl: String
): WebView {
    return WebView(context).apply {
        setBackgroundColor(android.graphics.Color.TRANSPARENT)
        settings.javaScriptEnabled = true
        settings.useWideViewPort = true
        settings.loadWithOverviewMode = false
        settings.apply {
            setSupportZoom(false)
            builtInZoomControls = false
            displayZoomControls = false
        }
        overScrollMode = WebView.OVER_SCROLL_NEVER
        isVerticalScrollBarEnabled = false
        isHorizontalScrollBarEnabled = false
        isScrollContainer = false
        // Touch handling: give JS a grace period to signal knob interaction
        // via setKnobActive(true) before deciding to release to parent scroll.
        var downTime = 0L
        var decided = false
        val gracePeriodMs = 150L
        setOnTouchListener { v, event ->
            when (event.action) {
                android.view.MotionEvent.ACTION_DOWN -> {
                    downTime = android.os.SystemClock.uptimeMillis()
                    decided = false
                    bridge.isKnobDragging = false
                }
                android.view.MotionEvent.ACTION_MOVE -> {
                    if (!decided) {
                        val elapsed = android.os.SystemClock.uptimeMillis() - downTime
                        if (bridge.isKnobDragging) {
                            // JS claimed the touch — keep it
                            decided = true
                            v.parent?.requestDisallowInterceptTouchEvent(true)
                        } else if (elapsed > gracePeriodMs) {
                            // Grace period expired, JS didn't claim — release to parent
                            decided = true
                            v.parent?.requestDisallowInterceptTouchEvent(false)
                        }
                        // During grace period: don't decide yet, keep touch
                    }
                }
                android.view.MotionEvent.ACTION_UP, android.view.MotionEvent.ACTION_CANCEL -> {
                    decided = false
                    v.parent?.requestDisallowInterceptTouchEvent(false)
                }
            }
            false // Don't consume — let WebView handle the event for JS
        }
        webViewClient = object : WebViewClient() {
            override fun shouldInterceptRequest(
                view: WebView?,
                request: WebResourceRequest?
            ): WebResourceResponse? {
                val url = request?.url ?: return null
                return assetLoader.shouldInterceptRequest(url) ?: super.shouldInterceptRequest(view, request)
            }
        }
        addJavascriptInterface(bridge, "AndroidHost")
        this.loadUrl(loadUrl)
    }
}

private fun buildAssetLoader(
    context: android.content.Context,
    baseDir: File,
    iconTemplate: String,
    templateData: ModguiTemplateData,
    pluginInfo: PluginInfo,
    inline: Boolean = false
): WebViewAssetLoader {
    return WebViewAssetLoader.Builder()
        .setDomain("modgui.app")
        .addPathHandler("/") { path ->
            val relativePath = path.trimStart('/')
            if (relativePath.startsWith("resources/")) {
                // Try shared modgui resources first
                val assetPath = "lv2/modgui_shared_resources/$relativePath"
                val sharedResult = try {
                    val bytes = context.assets.open(assetPath).use { it.readBytes() }
                    val ext = assetPath.substringAfterLast('.', "").lowercase()
                    val mimeType = when (ext) {
                        "png" -> "image/png"
                        "jpg", "jpeg" -> "image/jpeg"
                        "gif" -> "image/gif"
                        "svg" -> "image/svg+xml"
                        "css" -> "text/css"
                        else -> "application/octet-stream"
                    }
                    WebResourceResponse(mimeType, "UTF-8", bytes.inputStream())
                } catch (_: Exception) {
                    null
                }
                if (sharedResult != null) return@addPathHandler sharedResult
                // Fall through: map /resources/X → modgui/X in the plugin bundle
                // (modgui:resourcesDirectory <modgui> in TTL)
                val resourceFileName = relativePath.removePrefix("resources/")
                val bundleResourceFile = File(baseDir, "modgui/$resourceFileName")
                if (bundleResourceFile.exists() &&
                    bundleResourceFile.canonicalPath.startsWith(baseDir.canonicalPath)) {
                    val ext = bundleResourceFile.extension.lowercase()
                    val mimeType = when (ext) {
                        "png" -> "image/png"
                        "jpg", "jpeg" -> "image/jpeg"
                        "gif" -> "image/gif"
                        "svg" -> "image/svg+xml"
                        "css" -> "text/css"
                        else -> "application/octet-stream"
                    }
                    return@addPathHandler WebResourceResponse(mimeType, "UTF-8",
                        bundleResourceFile.inputStream())
                }
            }
            val file = File(baseDir, relativePath)
            if (!file.exists()) return@addPathHandler null
            if (!file.canonicalPath.startsWith(baseDir.canonicalPath)) return@addPathHandler null
            val isIconTemplate = relativePath == iconTemplate
            val mimeType = when (file.extension.lowercase()) {
                "html", "htm" -> "text/html"
                "css" -> "text/css"
                "js" -> "application/javascript"
                "png" -> "image/png"
                "jpg", "jpeg" -> "image/jpeg"
                "gif" -> "image/gif"
                "svg" -> "image/svg+xml"
                else -> "application/octet-stream"
            }
            val inputStream = when {
                isIconTemplate && (file.extension == "html" || file.extension == "htm") -> {
                    val raw = file.readText(Charsets.UTF_8)
                    val fragment = renderModguiTemplate(raw, templateData)
                    val cssName = iconTemplate.replace(Regex("""^.*/icon-(.+)\.html?$"""), "stylesheet-$1.css")
                    val portsJson = buildPortsJson(pluginInfo)
                    val scaleStyle = if (inline) {
                        """
                        <style>
                        html, body { margin: 0; padding: 0; background: transparent; overflow: hidden; touch-action: none; }
                        .modgui-inline-wrap {
                            position: absolute; top: 0; left: 0;
                            transform-origin: top left;
                            overflow: visible;
                        }
                        ::-webkit-scrollbar { display: none; }
                        </style>
                        <script>
                        var _naturalW = 0, _naturalH = 0;
                        window.addEventListener('load', function() {
                            var wrap = document.querySelector('.modgui-inline-wrap');
                            if (!wrap) return;
                            // Measure at no transform
                            wrap.style.transform = 'none';
                            var cw = 0, ch = 0;
                            // Prefer the mod-pedal element's explicit CSS dimensions
                            var pedal = wrap.querySelector('[class*="mod-pedal"]');
                            if (pedal && pedal.offsetWidth > 0 && pedal.offsetHeight > 0) {
                                cw = pedal.offsetWidth;
                                ch = pedal.offsetHeight;
                            } else {
                                // Fallback: measure all descendants
                                cw = wrap.scrollWidth;
                                ch = wrap.scrollHeight;
                                var base = wrap.getBoundingClientRect();
                                var children = wrap.querySelectorAll('*');
                                for (var i = 0; i < children.length; i++) {
                                    var cr = children[i].getBoundingClientRect();
                                    if (cr.width === 0 && cr.height === 0) continue;
                                    cw = Math.max(cw, cr.right - base.left);
                                    ch = Math.max(ch, cr.bottom - base.top);
                                }
                            }
                            _naturalW = cw;
                            _naturalH = ch;
                            if (cw > 0 && ch > 0) {
                                AndroidHost.setContentSize(Math.ceil(cw), Math.ceil(ch));
                                fitInline();
                            }
                        });

                        function fitInline() {
                            if (_naturalW === 0 || _naturalH === 0) return;
                            var wrap = document.querySelector('.modgui-inline-wrap');
                            if (!wrap) return;
                            var vw = document.documentElement.clientWidth;
                            var vh = document.documentElement.clientHeight;
                            var scale = Math.min(vw / _naturalW, vh / _naturalH);
                            var scaledW = _naturalW * scale;
                            var scaledH = _naturalH * scale;
                            var offsetX = (vw - scaledW) / 2;
                            var offsetY = (vh - scaledH) / 2;
                            wrap.style.transform = 'translate(' + offsetX + 'px, ' + offsetY + 'px) scale(' + scale + ')';
                        }

                        // Called from Android when container size changes
                        function scaleToFill() { fitInline(); }

                        window.addEventListener('resize', fitInline);
                        </script>
                        """
                    } else ""
                    val fullscreenWrapper = if (!inline) """
                        <style>
                        html, body { overflow: hidden; touch-action: none; margin: 0; padding: 0;
                            width: 100vw; height: 100vh; background: transparent; }
                        .modgui-center-wrap {
                            position: absolute; top: 0; left: 0;
                            transform-origin: top left;
                            overflow: visible;
                        }
                        ::-webkit-scrollbar { display: none; }
                        </style>
                        <script>
                        window.addEventListener('load', function() {
                            var wrap = document.querySelector('.modgui-center-wrap');
                            var el = wrap.firstElementChild;
                            if (!el) return;
                            var naturalW = 0, naturalH = 0;
                            function measure() {
                                wrap.style.transform = 'none';
                                var cw = 0, ch = 0;
                                // Prefer the mod-pedal element's explicit CSS dimensions
                                var pedal = wrap.querySelector('[class*="mod-pedal"]');
                                if (pedal && pedal.offsetWidth > 0 && pedal.offsetHeight > 0) {
                                    cw = pedal.offsetWidth;
                                    ch = pedal.offsetHeight;
                                } else {
                                    // Fallback: measure all descendants
                                    cw = wrap.scrollWidth;
                                    ch = wrap.scrollHeight;
                                    var base = wrap.getBoundingClientRect();
                                    var children = wrap.querySelectorAll('*');
                                    for (var i = 0; i < children.length; i++) {
                                        var cr = children[i].getBoundingClientRect();
                                        if (cr.width === 0 && cr.height === 0) continue;
                                        cw = Math.max(cw, cr.right - base.left);
                                        ch = Math.max(ch, cr.bottom - base.top);
                                    }
                                }
                                naturalW = cw;
                                naturalH = ch;
                                document.title = 'content=' + cw + 'x' + ch;
                            }
                            function fit() {
                                if (naturalW === 0) measure();
                                if (naturalW === 0 || naturalH === 0) return;
                                var vw = document.documentElement.clientWidth;
                                var vh = document.documentElement.clientHeight;
                                var scale = Math.min(vw / naturalW, vh / naturalH) * 0.85;
                                var scaledW = naturalW * scale;
                                var scaledH = naturalH * scale;
                                var offsetX = (vw - scaledW) / 2;
                                var offsetY = (vh - scaledH) / 2;
                                wrap.style.transform = 'translate(' + offsetX + 'px, ' + offsetY + 'px) scale(' + scale + ')';
                                document.title = 'content=' + naturalW + 'x' + naturalH + ' vp=' + vw + 'x' + vh + ' scale=' + scale.toFixed(3);
                            }
                            measure();
                            fit();
                            window.addEventListener('resize', function() {
                                fit();
                            });
                        });
                        </script>
                        <div class="modgui-center-wrap">
                        $fragment
                        </div>
                    """ else """<div class="modgui-inline-wrap">$fragment</div>"""
                    val viewportMeta = """<meta name="viewport" content="width=1200, initial-scale=1, user-scalable=no">"""
                    val bodyStyle = if (inline) {
                        """style="margin:0; padding:0; background:transparent;" """
                    } else ""
                    val fullHtml = """
                        <!DOCTYPE html>
                        <html>
                        <head>
                        <meta charset="UTF-8">
                        $viewportMeta
                        <link rel="stylesheet" href="$cssName">
                        <style>
                        /* Disable Android WebView tap highlight and text selection */
                        * { -webkit-tap-highlight-color: transparent; -webkit-user-select: none; user-select: none; }
                        /* Fix z-index stacking: ensure controls render above background elements */
                        .mod-control-group { z-index: 10; }
                        .mod-knob { overflow: visible !important; }
                        .mod-pedal-input, .mod-pedal-output { z-index: 5; }
                        /* Bypass indicator light — CSS-only, no external PNG required */
                        .mod-pedal .mod-light { display: flex; align-items: center; justify-content: center; }
                        .mod-pedal .mod-light::after { content: ''; width: 14px; height: 14px; border-radius: 50%; display: block; }
                        .mod-pedal .mod-light.on::after  { background: radial-gradient(circle at 35% 35%, #ff6060, #cc0000); box-shadow: 0 0 6px 2px rgba(200,0,0,0.7); }
                        .mod-pedal .mod-light.off::after { background: radial-gradient(circle at 35% 35%, #552222, #331111); }
                        </style>
                        $scaleStyle
                        </head>
                        <body $bodyStyle>
                        $fullscreenWrapper
                        <script>
                        ${modguiRuntimeJs(context, portsJson)}
                        </script>
                        </body>
                        </html>
                    """.trimIndent()
                    fullHtml.toByteArray(Charsets.UTF_8).inputStream()
                }
                file.extension.lowercase() == "css" -> {
                    var css = file.readText(Charsets.UTF_8)
                    css = css.replace("{{{cns}}}", "")
                    css = css.replace("{{{ns}}}", "")
                    css = Regex("""@import\s+url\s*\(\s*[^)]*fonts[^)]*\)\s*;?\s*""").replace(css, "/* fonts not in bundle */\n")
                    css.toByteArray(Charsets.UTF_8).inputStream()
                }
                else -> file.inputStream()
            }
            WebResourceResponse(mimeType, "UTF-8", inputStream)
        }
        .build()
}

private fun buildPortsJson(pluginInfo: PluginInfo): String {
    val entries = pluginInfo.controlPorts.joinToString(",") { p ->
        """{"symbol":"${p.symbol.replace("\"", "\\\"")}","min":${p.minValue},"max":${p.maxValue},"default":${p.defaultValue},"toggle":${p.isToggle}}"""
    }
    return "[$entries]"
}

/** Cache for modgui_runtime.js template loaded from assets. */
private var modguiRuntimeJsTemplate: String? = null

private fun modguiRuntimeJs(context: android.content.Context, portsJson: String): String {
    val template = modguiRuntimeJsTemplate ?: run {
        context.assets.open("modgui_runtime.js").bufferedReader().use { it.readText() }
            .also { modguiRuntimeJsTemplate = it }
    }
    return template.replace("\$PORTS_JSON", portsJson)
}
