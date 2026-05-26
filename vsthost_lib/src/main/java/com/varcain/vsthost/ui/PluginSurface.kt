package com.varcain.vsthost.ui

import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.Log
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.ViewConfiguration
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.content.res.Configuration
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.viewinterop.AndroidView
import com.varcain.vsthost.NativeBridge
import com.varcain.vsthost.util.X11Keymap

/** Render one plugin's editor.
 *
 *  The SurfaceView's aspect ratio is FIXED at mount time. Changing it on
 *  the fly re-fires surfaceChanged and triggers Adreno-side EGL races
 *  (observed SIGSEGV inside libGLESv2_adreno.so during eglSwapBuffers).
 *
 *  The X11 server's renderer letterboxes the plugin's framebuffer inside
 *  the surface and `injectTouch` reverses the same scale/offset, so any
 *  fixed aspect ratio works — the plugin just gets centered with black
 *  bars when its native size doesn't match the viewport. */
@Composable
fun PluginSurface(
    displayNumber: Int,
    pluginWidth: Int,
    pluginHeight: Int,
    modifier: Modifier = Modifier,
    /** True = destroy the X11 display when this composable disposes
     *  (vstpoc standalone — when the user closes the editor, the wine
     *  subprocess is also being torn down). False = keep the display
     *  alive across recompositions (GuitarRackCraft inline-in-rack —
     *  the wine subprocess outlives any single activity transition,
     *  and destroying the display would orphan its X11 client). */
    destroyOnDispose: Boolean = true,
) {
    DisposableEffect(displayNumber) {
        onDispose {
            EditorViewRegistry.unregister(displayNumber)
            if (destroyOnDispose) {
                NativeBridge.nativeDetachAndDestroyX11Display(displayNumber)
            }
        }
    }

    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val aspect = pluginWidth.toFloat() / pluginHeight.toFloat()
    Box(
        modifier = if (isLandscape) modifier.fillMaxSize() else modifier.fillMaxWidth(),
        contentAlignment = Alignment.Center,
    ) {
        AndroidView(
            modifier = if (isLandscape) {
                Modifier.fillMaxSize().aspectRatio(aspect, matchHeightConstraintsFirst = true)
            } else {
                Modifier.fillMaxWidth().aspectRatio(aspect)
            },
            factory = { ctx ->
                EditorSurfaceView(ctx, displayNumber).also {
                    EditorViewRegistry.register(displayNumber, it)
                }
            },
        )
    }
}

/** Process-wide map of `displayNumber → EditorSurfaceView` so external code
 *  (e.g. GuitarRackCraft's rack chrome) can request soft-IME against the
 *  inline editor without knowing how PluginSurface is composed. The view
 *  registers itself in PluginSurface's factory and unregisters on dispose.
 *
 *  Lives in the vsthost_lib package so it stays scoped to the in-process
 *  X11 server's display numbers (which are owned by vsthost, not :app's
 *  X11DisplayManager). */
object EditorViewRegistry {
    private val views = mutableMapOf<Int, EditorSurfaceView>()

    @Synchronized
    fun register(displayNumber: Int, view: EditorSurfaceView) {
        views[displayNumber] = view
    }

    @Synchronized
    fun unregister(displayNumber: Int) {
        views.remove(displayNumber)
    }

    @Synchronized
    fun showKeyboard(displayNumber: Int): Boolean {
        val v = views[displayNumber] ?: return false
        v.requestFocus()
        val imm = v.context.getSystemService(InputMethodManager::class.java)
        imm?.showSoftInput(v, InputMethodManager.SHOW_IMPLICIT)
        return true
    }
}

/** SurfaceView that:
 *  - Forwards touch events to the X11 server display.
 *  - Captures hardware/soft-IME key events and forwards them via
 *    NativeBridge.nativeInjectX11Key + the X11Keymap translation table.
 *
 *  The IME path: Android shows the soft keyboard against this view when
 *  someone calls showSoftInput. The IME asks the view for an
 *  InputConnection via onCreateInputConnection; we hand back a
 *  BaseInputConnection whose commitText / sendKeyEvent / deleteSurroundingText
 *  routes characters to nativeInjectX11Key. Most Android soft IMEs use
 *  commitText (rather than synthetic key events) so the BaseInputConnection
 *  overrides are essential — onKeyDown alone wouldn't see soft-keyboard input. */
class EditorSurfaceView(
    context: Context,
    private val displayNumber: Int,
) : SurfaceView(context) {

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        // Forward touches to the X server. Single-finger short tap/drag maps
        // to left button (action 0/1/2). A LONG-PRESS without moving past
        // the touch slop converts the gesture into a right-click (action=3
        // — atomic right press+release) at the original press location, then
        // swallows the rest of the gesture so no spurious drag/release events
        // follow.
        //
        // Long-press timeout uses Android's ViewConfiguration default
        // (~500 ms). Touch slop also comes from ViewConfiguration so we
        // cancel the long-press if the finger drifts more than the slop
        // distance (signaling a drag, not a press-and-hold).
        val longPressMs = ViewConfiguration.getLongPressTimeout().toLong()
        val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
        val handler = Handler(Looper.getMainLooper())
        var rightClickConsumed = false
        var downX = 0
        var downY = 0
        var leftButtonDownSent = false
        var pendingLongPress: Runnable? = null
        setOnTouchListener { view, e ->
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    rightClickConsumed = false
                    leftButtonDownSent = false
                    downX = e.x.toInt()
                    downY = e.y.toInt()
                    requestFocus()
                    // Block the parent (LazyColumn in GuitarRackCraft's rack)
                    // from intercepting drags as scroll gestures — otherwise
                    // turning a knob just scrolls the rack instead. Harmless
                    // when the parent doesn't intercept (vstpoc standalone).
                    view.parent?.requestDisallowInterceptTouchEvent(true)
                    // DEFER the left ButtonPress — we don't know yet whether
                    // this becomes a tap (send left) or a long-press (send
                    // right instead). If long-press fires we want to avoid
                    // ever telling wine a left button went down.
                    val pressX = downX
                    val pressY = downY
                    pendingLongPress = Runnable {
                        rightClickConsumed = true
                        pendingLongPress = null
                        NativeBridge.nativeInjectX11Touch(displayNumber, 3, pressX, pressY)
                    }
                    handler.postDelayed(pendingLongPress!!, longPressMs)
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (e.x.toInt() - downX)
                    val dy = (e.y.toInt() - downY)
                    val moved = dx * dx + dy * dy > touchSlop * touchSlop
                    if (rightClickConsumed) {
                        // Right-click already fired; swallow further motion.
                    } else if (moved) {
                        // It's a drag, not a long-press. Cancel the pending
                        // right-click and start the deferred left button if
                        // not started yet.
                        pendingLongPress?.let { handler.removeCallbacks(it) }
                        pendingLongPress = null
                        if (!leftButtonDownSent) {
                            leftButtonDownSent = true
                            NativeBridge.nativeInjectX11Touch(displayNumber, 0, downX, downY)
                        }
                        NativeBridge.nativeInjectX11Touch(displayNumber, 2, e.x.toInt(), e.y.toInt())
                    }
                    // else: still within slop, keep waiting for long-press.
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    pendingLongPress?.let { handler.removeCallbacks(it) }
                    pendingLongPress = null
                    if (rightClickConsumed) {
                        // Right-click already fired atomically; nothing to do.
                    } else if (leftButtonDownSent) {
                        // We were dragging — close the gesture with a left UP.
                        NativeBridge.nativeInjectX11Touch(displayNumber, 1, e.x.toInt(), e.y.toInt())
                    } else if (e.actionMasked == MotionEvent.ACTION_UP) {
                        // Short tap — never sent the DOWN yet. Send press+release
                        // back-to-back at the press location so JUCE sees a
                        // single click (not a hover).
                        NativeBridge.nativeInjectX11Touch(displayNumber, 0, downX, downY)
                        NativeBridge.nativeInjectX11Touch(displayNumber, 1, downX, downY)
                    }
                    leftButtonDownSent = false
                    rightClickConsumed = false
                }
                MotionEvent.ACTION_POINTER_DOWN,
                MotionEvent.ACTION_POINTER_UP -> {
                    // Ignore multi-touch — two-finger right-click is replaced
                    // by long-press above.
                }
                else -> return@setOnTouchListener false
            }
            true
        }
        holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(h: SurfaceHolder) {}
            override fun surfaceChanged(h: SurfaceHolder, fmt: Int, w: Int, hh: Int) {
                NativeBridge.nativeAttachSurfaceToX11Display(displayNumber, h.surface, w, hh)
            }
            override fun surfaceDestroyed(h: SurfaceHolder) {}
        })
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        // TYPE_CLASS_TEXT with TYPE_TEXT_FLAG_NO_SUGGESTIONS: regular
        // keyboard with clipboard bar available, autocomplete/suggestions
        // disabled. Gboard still uses composing-text for some characters,
        // but our setComposingText/commitText track the composing buffer
        // and forward only the delta (added chars + backspaces) to X11.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_ACTION_DONE or
            EditorInfo.IME_FLAG_NO_FULLSCREEN or
            EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
        return KeyForwardingInputConnection(this, displayNumber)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val mapping = X11Keymap.fromAndroidKey(keyCode, event.metaState)
            ?: return super.onKeyDown(keyCode, event)
        NativeBridge.nativeInjectX11Key(displayNumber, 0, mapping.keycode, mapping.state)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val mapping = X11Keymap.fromAndroidKey(keyCode, event.metaState)
            ?: return super.onKeyUp(keyCode, event)
        NativeBridge.nativeInjectX11Key(displayNumber, 1, mapping.keycode, mapping.state)
        return true
    }
}

/** InputConnection wired to nativeInjectX11Key. Soft-keyboard input
 *  arrives here via commitText (most common), sendKeyEvent (some IMEs),
 *  or deleteSurroundingText (backspace). Each gets translated to a
 *  KeyPress + KeyRelease pair on the X11 server.
 *
 *  Inherits BaseInputConnection's plumbing for selection, composing text,
 *  etc. — we don't pretend to actually edit a text buffer, we just turn
 *  the IME's intents into key events. */
private class KeyForwardingInputConnection(
    targetView: EditorSurfaceView,
    private val displayNumber: Int,
) : BaseInputConnection(targetView, /*fullEditor=*/false) {

    // Tracks the current composing buffer (Gboard's in-flight composition).
    // setComposingText replaces this entirely; we send the diff to X11.
    private var composing: String = ""

    // Gboard's autofill / clipboard suggestion can fire commitText with the
    // same value repeatedly when the user taps Login or moves focus. Each
    // commitText is APPEND to the target text field (the field doesn't get
    // cleared between commits), so a "1234" password becomes "12341234..."
    // after a few clicks. Suppress duplicate commits within a short window.
    private var lastCommit: String = ""
    private var lastCommitAt: Long = 0
    private val dedupeWindowMs: Long = 1500

    /** Send X11 BackSpace N times. */
    private fun sendBackspaces(n: Int) {
        repeat(n) {
            NativeBridge.nativeInjectX11Key(displayNumber, 0, 53, 0)
            NativeBridge.nativeInjectX11Key(displayNumber, 1, 53, 0)
        }
    }

    /** Send characters as KeyPress + KeyRelease pairs. */
    private fun sendChars(s: CharSequence) {
        s.forEach { ch ->
            val m = X11Keymap.lookup(ch)
            if (m != null) {
                NativeBridge.nativeInjectX11Key(displayNumber, 0, m.keycode, m.state)
                NativeBridge.nativeInjectX11Key(displayNumber, 1, m.keycode, m.state)
            } else {
                Log.i("IMEForward", "  no mapping for char=${ch.code} '${ch}'")
            }
        }
    }

    /** Replace `composing` with `newText` on the X11 side by sending the diff. */
    private fun replaceComposingWith(newText: String) {
        val common = newText.commonPrefixWith(composing).length
        sendBackspaces(composing.length - common)
        sendChars(newText.substring(common))
    }

    override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val newText = text?.toString() ?: ""
        val now = System.currentTimeMillis()
        // Suppress duplicate Gboard autofill: if the same text was committed
        // within the dedupe window, the field probably already has it (Gboard
        // re-fires commitText on focus changes / Login button taps). Skip the
        // resend so the field doesn't get the password concatenated multiple
        // times.
        if (newText.length >= 2 && newText == lastCommit && (now - lastCommitAt) < dedupeWindowMs) {
            Log.i("IMEForward", "commitText DEDUPED len=${newText.length} text='${newText}' Δt=${now - lastCommitAt}ms")
            // Still clear composing so IME state is consistent.
            composing = ""
            return true
        }
        Log.i("IMEForward", "commitText len=${newText.length} text='${newText}' composing.len=${composing.length}")
        // commitText finalizes whatever the user typed. If we had composing
        // text in flight, the committed text REPLACES it. Compute the diff
        // from current composing to commit-text, then clear composing.
        replaceComposingWith(newText)
        composing = ""
        lastCommit = newText
        lastCommitAt = now
        return true
    }

    override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
        val newText = text?.toString() ?: ""
        Log.i("IMEForward", "setComposingText len=${newText.length} text='${newText}' composing.len=${composing.length}")
        // Gboard sends growing/shrinking composing strings as user types.
        // Diff-send to X11 and remember the new composing buffer.
        replaceComposingWith(newText)
        composing = newText
        return true
    }

    override fun finishComposingText(): Boolean {
        Log.i("IMEForward", "finishComposingText composing.len=${composing.length}")
        // IME closes composition without changing text. The chars in
        // `composing` were already sent to X11; just clear local state.
        composing = ""
        return true
    }

    override fun sendKeyEvent(event: KeyEvent): Boolean {
        val mapping = X11Keymap.fromAndroidKey(event.keyCode, event.metaState)
        Log.i("IMEForward", "sendKeyEvent keyCode=${event.keyCode} action=${event.action} mapping=${mapping}")
        if (mapping == null) return false
        val action = if (event.action == KeyEvent.ACTION_UP) 1 else 0
        NativeBridge.nativeInjectX11Key(displayNumber, action, mapping.keycode, mapping.state)
        return true
    }

    override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
        // Most IMEs use this for the Backspace button rather than
        // sending a KEYCODE_DEL. Translate to N BackSpace events.
        // Also shrink our composing buffer if it was in flight.
        Log.i("IMEForward", "deleteSurroundingText before=$beforeLength after=$afterLength composing.len=${composing.length}")
        sendBackspaces(beforeLength)
        if (composing.isNotEmpty()) {
            composing = composing.dropLast(beforeLength.coerceAtMost(composing.length))
        }
        return true
    }

    override fun performEditorAction(actionCode: Int): Boolean {
        // IME's "Done" → Enter on the plugin.
        NativeBridge.nativeInjectX11Key(displayNumber, 0, 52, 0)  // 52 = Return
        NativeBridge.nativeInjectX11Key(displayNumber, 1, 52, 0)
        return true
    }

    override fun performContextMenuAction(id: Int): Boolean = false
    override fun performPrivateCommand(action: String?, data: Bundle?): Boolean = false
}
