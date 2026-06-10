# Migration: retire the WS_CHILD activation patch stack (VST2 top-level host frame)

## Context

Goal: **retire wine patches `0037`–`0040`** (the "top-level WS_CHILD activation" stack). They
are pure complexity and a maintenance tax on every wine rebuild.

Investigation (2026-06-08) overturned the "virtual desktop" framing:

- **There is no wine virtual-desktop in play.** `WineSetup.applyVirtualDesktopRegistry`
  (`vsthost_lib/.../wine/WineSetup.kt:1414`, the `Desktops "vstpoc"="4096x2160"` seed) is
  **dead code — never called**. The `4096×2160` is only the X-server's screen/framebuffer
  placeholder (shrinks to editor size via slot-promotion).
- The patches exist **solely** because the **VST2 host** (`vst_host.c`) calls
  `effEditOpen(GetDesktopWindow())`, so the plugin makes its editor a **`WS_CHILD` of the
  desktop** ("top-level WS_CHILD"), which wine refuses to activate. Confirmed verbatim by
  patch `0040`'s own comment ("vst_host effEditOpen passes the desktop as the parent HWND…").
- The **VST3 host** (`vst3_host.cpp:1318`) already creates a top-level `WS_POPUP` host window
  and attaches the editor as its child → it never trips the patches.
- **`vst_host.c` already has the host-frame machinery** (`VSTPOC_HOST_FRAME`,
  `vst_host.c:379-433`), used by the PC launcher but **disabled on Android** (defaults to
  `parent_hwnd = desktop`, `vst_host.c:384`).

So the migration is small in surface area: **make the VST2 host parent editors under a
chromeless top-level host window on Android (like VST3 already does), then remove the four
patches.** This eliminates a whole class of activation complexity and does NOT touch the
caret (that's FEX, orthogonal).

## Current state (key files)

- `vsthost_lib/external/vst_host/vst_host.c`
  - `:343` `effEditGetRect` → editor size `w,h` (already queried before the frame).
  - `:379-433` `VSTPOC_HOST_FRAME` block: registers a class, `AdjustWindowRect` + a
    **decorated** `WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|…` frame (PC chrome), sets
    `parent_hwnd = frame_hwnd`. **Disabled on Android** (env unset → `parent_hwnd = desktop`).
  - `:436` `effEditOpen(parent_hwnd)`.
- `vsthost_lib/external/vst_host_vst3/vst3_host.cpp:1304-1334` — the reference pattern:
  `WS_POPUP` (chromeless), size = editor, `ShowWindow`, then `view->attached(parent)`.
- `vsthost_lib/patches/wine/003{7,8,9}-*.patch`, `0040-wineserver-foreground-ws-child.patch`
  — the four WS_CHILD-of-desktop activation patches to retire. (`0041-winex11-keyevent-diag`
  is a pure diagnostic, separate.)
- `vsthost_lib/src/main/cpp/launcher/WineHostProcess.cpp` — env plumbing (note the **dup-env
  caveat**: vars must be added to BOTH `setupWineEnvChild` and the inline block in `start()`).
- Build: `vsthost_lib/scripts/build-vst-host.sh` (→ `vst_host.exe`/`vst_host_x86.exe`);
  the wine rebuild/pack path for patch removal.

## The change

**1. Android-appropriate host frame in `vst_host.c`.** Add a chromeless mode to the existing
frame block: when targeting Android, create the frame as **`WS_POPUP`**, **no
`AdjustWindowRect`** (frame client == editor size → fills the SurfaceView 1:1, no letterbox
change), no caption/title. Mirror `vst3_host.cpp`'s `WS_POPUP` host window. Reuse the existing
`ensure_host_frame_class_registered` + cascade logic. Gate via a new value, e.g.
`VSTPOC_HOST_FRAME=popup` (keep `=1` = the PC decorated frame), so PC behaviour is unchanged.

**2. Enable it on Android.** Set the env in `WineHostProcess.cpp` (BOTH env blocks). The
editor then parents under the frame (`parent != desktop`) → wine activates the frame as a
normal top-level window and focus flows to the child editor via the standard Win32 chain.

**3. Remove the patches.** Delete `0037`–`0040` and rebuild wine. With no editor parented
under the desktop, their `parent == desktop` condition can never match, so they are inert
before removal and unneeded after.

## Phased, de-risked execution

- **Phase 1 — frame, patches still present (proves the approach):**
  1. Add the `WS_POPUP` Android frame mode to `vst_host.c`; build `vst_host.exe`.
  2. Wire the env in `WineHostProcess.cpp`; build+install.
  3. **Verify the X server renders frame→editor** (the #1 risk — see below).
  4. Verify VST2 plugins activate + accept text **with patches still in** (now inert).
- **Phase 2 — retire the patches:**
  1. Remove `0037`–`0040`; rebuild/pack wine; install.
  2. Re-verify VST2 plugins still work (patches confirmed unneeded).
  3. Regression VST3 (BIAS/TONEX) — expected unaffected (already host-window based).

## Risks & how to check them

1. **X-server rendering of frame→editor (highest risk).** `vst_host.c:382` notes Android
   used desktop-parent "so the editor renders straight into the embedded SurfaceView." With a
   frame, the editor is a child of the frame and the X server must composite the frame's
   content. VST3 already works exactly this way (host window → child editor → slot), so the
   slot-promotion + child-window PutImage paths in `X11NativeDisplay.cpp` should handle it —
   but **confirm first** with one plugin (watch for an empty frame / black editor). If the
   slot logic picks the frame but never the child's pixels, the fix is in the
   CreateWindow/PutImage slot-promotion (it already supports child-of-slot for VST3).
2. **Per-plugin VST2 quirks.** JUCE VST2 editors may handle a frame parent vs desktop parent
   differently (focus, sizing). Test the suite.
3. **Other consumers of the patches.** Checked: only `vst_host.c` parents under the desktop;
   `uihost_stub.c` doesn't create editor windows. Low risk, but grep before deleting.
4. **Frame sizing.** Use `effEditGetRect` size, `WS_POPUP`, **no** `AdjustWindowRect` — the
   frame must equal the editor size so the existing 1:1 surface mapping and touch-coordinate
   math (`injectTouch`/`drainTouchQueue`) are unchanged.

## Verification (end-to-end)

- **Canonical repro: X50II (JUCE VST2)** — its Username text field is the original reason the
  activation patches exist. **Success = the caret appears and typing lands in X50II with the
  host frame and WITHOUT patches 0037–0040.**
- Slew2 + WagnerSharp (VST2): render + audio + knob drags still work.
- Regression: a VST3 plugin (BIAS or TONEX) renders/clicks unchanged.
- Confirm `git grep` shows no remaining reference to the removed patches in build scripts.

## Effort

~2–4 days: the frame machinery already exists, so it's a style/gating tweak + env wiring +
`vst_host.exe` build + the X-server render check + a VST2-suite regression pass + one wine
rebuild to drop the patches. Lower risk than a from-scratch window-scheme change because VST3
already proves the host-window pattern on this exact X server.

## Out of scope

The BIAS caret (FEX miscompile) — orthogonal; resume that via the differential branch-trace
([[feedback_fex_custom_build_deploy]]). This migration neither helps nor hinders it.
