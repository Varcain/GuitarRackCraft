# Archived wine patches

Patches that were active in the build at some point but are no longer applied
to `external/wine-upstream/`. Kept here for historical reference (and so we can
revisit if a similar deadlock pattern resurfaces).

## 020 — `RtlSleepConditionVariableSRW` / `RtlWaitOnAddress` sent-message drain

Originally added to break a circular SendMessage deadlock in JUCE-7 popups
(TH-U's rig-pack selection box). Patched `dlls/ntdll/sync.c` to poll the
wait at 200ms intervals when timeout was infinite, and to drain pending
sent messages on every wake (timeout OR alerted) when the requested
timeout was ≤ 500ms.

Removed 2026-05-22: replaced by server-side cycle detector
[`025-server-cycle-detect-synthesize.patch`](../025-server-cycle-detect-synthesize.patch),
which solves the same deadlock by synthesizing a 0-reply for focus
messages stuck > 500ms.

The 020 drain dispatched cross-thread sent messages from inside TH-U's
WindowProc whenever it did its own short-timeout thread sync. That
out-of-order dispatch left TH-U's drag-drop state with a NULL field
pointer; the next access crashed at TH-U+0x1802DF0C3 (NULL `this` read at
offset 0x10). With 025 covering the popup path, 020 was both redundant
and actively harmful.

Note: a narrowed variant `020-cv-drain-sent-messages-narrow.patch` lives
in `../` (active tree). It only drains on STATUS_TIMEOUT, not on
STATUS_ALERTED. That keeps the rendering-deadlock fix without
re-introducing the drag-drop crash.

## 013 — `NtUserSetFocus` allow top-level WS_CHILD

Originally added to fix JUCE TextField WM_CHAR delivery: upstream wine's
`NtUserSetFocus` returns 0 for WS_CHILD windows whose parent is the
desktop (which is how vst_host hosts plugin editors). JUCE's
`grabKeyboardFocus → SetFocus` got 0 back, never committed internal
focus, dropped typed characters.

Removed 2026-05-22 as a candidate cause of the intermittent
TH-U+0x1802A05AF crash: 013 extends a focus path that 015
(`NtUserSetFocus` cross-thread auto-attach) and 014/021 (mouseactivate +
no-WM_SETFOCUS for top_level_child) likely already cover. Reverting 013
to isolate whether it contributes to the focus/destruction race seen on
TH-U workers.

Re-introduce if JUCE text-input regresses (license dialog typing, plugin
search boxes, save-preset name field, etc.).
