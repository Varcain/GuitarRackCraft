package com.varcain.vsthost.util

/**
 * ASCII char → (X11 hardware keycode, modifier state) lookup table that
 * matches the keymap declared in `app/src/main/cpp/x11/X11NativeDisplay.cpp`
 * (`kJavaKeymapPayload`).
 *
 *   Keycodes 8..17 = '0'..'9'  (shift gives ')!@#$%^&*(' )
 *   Keycodes 24..49 = 'a'..'z' (shift gives uppercase)
 *   Keycodes 50..63 = symbols + control keys (see below)
 *
 * Modifier values are X11 state bits:
 *   Shift = 0x01
 *   Control = 0x04
 *   Mod1/Alt = 0x08
 *
 * Returns null for characters we don't know how to type with this layout.
 * Soft-IME `commitText` strings should be sent one char at a time through
 * `lookup` — each entry produces one KeyPress + KeyRelease pair.
 */
object X11Keymap {
    const val SHIFT = 0x01
    const val CONTROL = 0x04

    data class Mapping(val keycode: Int, val state: Int)

    /** Map an ASCII codepoint to a keycode+modifier. */
    fun lookup(ch: Char): Mapping? = lookup(ch.code)
    fun lookup(unicode: Int): Mapping? {
        if (unicode in 'a'.code..'z'.code) return Mapping(24 + (unicode - 'a'.code), 0)
        if (unicode in 'A'.code..'Z'.code) return Mapping(24 + (unicode - 'A'.code), SHIFT)
        if (unicode in '0'.code..'9'.code) return Mapping(8 + (unicode - '0'.code), 0)
        return when (unicode) {
            ' '.code  -> Mapping(50, 0)
            '\t'.code -> Mapping(51, 0)
            '\n'.code -> Mapping(52, 0)
            '\r'.code -> Mapping(52, 0)
            8         -> Mapping(53, 0)   // Backspace
            127       -> Mapping(54, 0)   // Delete
            '.'.code  -> Mapping(55, 0)
            '>'.code  -> Mapping(55, SHIFT)
            ','.code  -> Mapping(56, 0)
            '<'.code  -> Mapping(56, SHIFT)
            '/'.code  -> Mapping(57, 0)
            '?'.code  -> Mapping(57, SHIFT)
            '@'.code  -> Mapping(58, 0)
            '-'.code  -> Mapping(59, 0)
            '_'.code  -> Mapping(59, SHIFT)
            ':'.code  -> Mapping(60, 0)
            ';'.code  -> Mapping(61, 0)
            '\''.code -> Mapping(62, 0)
            '"'.code  -> Mapping(62, SHIFT)
            27        -> Mapping(63, 0)   // Escape
            // Shifted digit row.
            '!'.code  -> Mapping(9, SHIFT)
            '#'.code  -> Mapping(11, SHIFT)
            '$'.code  -> Mapping(12, SHIFT)
            '%'.code  -> Mapping(13, SHIFT)
            '^'.code  -> Mapping(14, SHIFT)
            '&'.code  -> Mapping(15, SHIFT)
            '*'.code  -> Mapping(16, SHIFT)
            '('.code  -> Mapping(17, SHIFT)
            ')'.code  -> Mapping(8, SHIFT)
            else -> null
        }
    }

    /** Map a hardware Android `KeyEvent.KEYCODE_*` to a keycode+modifier.
     *  Used for hardware/Bluetooth keyboards where `onKeyDown` fires
     *  with a real keycode (not via InputConnection.commitText). */
    fun fromAndroidKey(androidKey: Int, metaState: Int): Mapping? {
        val shift = if (metaState and 0x41 != 0) SHIFT else 0  // META_SHIFT_ON
        val ctrl  = if (metaState and 0x1000 != 0) CONTROL else 0  // META_CTRL_ON
        val mods = shift or ctrl
        // Android keycode constants from android.view.KeyEvent.
        return when (androidKey) {
            in 29..54 -> Mapping(24 + (androidKey - 29), mods)        // KEYCODE_A..Z
            in 7..16  -> Mapping(8 + (androidKey - 7), mods)          // KEYCODE_0..9
            62 -> Mapping(50, mods)   // SPACE
            61 -> Mapping(51, mods)   // TAB
            66 -> Mapping(52, mods)   // ENTER
            67 -> Mapping(53, mods)   // DEL (backspace)
            112 -> Mapping(54, mods)  // FORWARD_DEL
            56 -> Mapping(55, mods)   // PERIOD
            55 -> Mapping(56, mods)   // COMMA
            76 -> Mapping(57, mods)   // SLASH
            77 -> Mapping(58, mods)   // AT
            69 -> Mapping(59, mods)   // MINUS
            74 -> Mapping(60, mods)   // SEMICOLON  (X11 layout treats keycode 60 as ":" — close enough)
            75 -> Mapping(62, mods)   // APOSTROPHE
            111 -> Mapping(63, mods)  // ESCAPE
            else -> null
        }
    }
}
