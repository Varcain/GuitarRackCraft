# iLok / PACE under wine-ARM64EC + FEX — investigation archive (PARKED 2026-06-16)

Running iLok/PACE-protected Windows plugins (Neural DSP **Granophyre**, the **iLok
License Manager**, the **PaceLicenseDServices / `LDSvc.exe`** daemon) on Android via
wine 11.9 (ARM64EC) + FEX-Emu. They install fine but crash in PACE DRM at runtime
under FEX (run fine on native x86-64 wine). This directory parks that work in a
resumable state. The **installer** half is solved and shipped; the **runtime** half
is prototyped but not shippable.

## TL;DR status
| Item | State | Where |
|---|---|---|
| iLok "License Support" MSI installs end-to-end | ✅ SHIPPED | `patches/wine/0054-0056` + WineSetup/WineHostProcess/VstInstallerViewModel (mainline) |
| LM/Granophyre/daemon **run** under FEX | ⏸ PARKED prototype | this dir |
| Crash #2 (rpcrt4 `NdrContextHandleUnmarshall`) | ✅ fixed (validated) | `patches/wine-rpcrt4-ndrclientcall2-x4-gap.PARKED.patch` |
| Crash #1 (`pace_wrapping_fi` stack scan) | ✅ fixed (validated, **fragile**) | `patches/fex-exitfunctionec-x4-gap.PARKED.patch` |
| Deeper layer (null-read loop → stack overflow) | ❌ open wall | — |
| iLok dongle/cloud **activation** | ❌ not attempted | Tonocracy = online token, not file-transferable |

## Root cause (web-confirmed: Hangover issue #225)
https://github.com/AndreRH/hangover/issues/225 — a Bose Professional dev hit the
EXACT same `LDSvc.exe`-under-wine-ARM64EC+FEX problem. **FEX's `ExitFunctionEC`
(`Source/Windows/ARM64EC/Module.S`) leaves `x4` 0x20 short for variadic ARM64EC calls
from emulated x86_64 when the call lazy-loads a DLL** (an extra stack gap shifts SP).
The variadic thunk's `stp x2, x3, [x4, #-0x10]!` then writes x2/x3 to the wrong stack
slot, corrupting the caller frame. MS ARM64EC ABI says x4 must point at the 5th
(first in-stack) parameter, past the 0x20 shadow space — exactly what FEX omits.
AndreRH wanted the fix in FEX's `ExitFunctionEC`; **nobody ever wrote it** (issue
still open). We did (the PARKED prototype here) — first of its kind.

## The two runtime crashes (both this root)
1. **Crash #2 — wine rpcrt4 `NdrContextHandleUnmarshall`, c0000005 write to 0x10.**
   PACE RPCs into the daemon; the client stub gets a garbage out-context-handle.
   Fixed by the hand-written ARM64EC variadic thunks (`NdrClientCall2`,
   `NdrAsyncClientCall`) detecting the gap (`*(x4+8)==0x10`) and `add x4,#0x20`.
   Port of Hangover commit `cc6dbd8`. Validated on-device: the 0x10 crash is gone.
2. **Crash #1 — PACE `pace_wrapping_fi`, c0000005 write to StackBase+0x18.**
   A loop scans the stack in 0x40 strides for a 0xCCCCCCCCCCCCCCCC sentinel (MSVC
   /RTC fill) and overruns because a wrong-x4 `stp` from a **compiler-generated**
   variadic thunk (CRT printf-family PACE calls) corrupted the scanned region.
   Compiler thunks can't be patched in wine source → fixed at FEX `ExitFunctionEC`
   (the PARKED prototype). VALIDATED: clears the crash, daemon runs all of PACE init
   (5.5KB → 3.1MB log).

## Why the FEX fix is PARKED, not shipped
- **Layout-sensitive**: `VSTPOC_STACK_PCT` (bigger stack) re-breaks crash #1 — the
  `*(x4+0x28)==0x10` detection / 0x20 gap isn't robust across stack layouts.
- **Global** change to every emulated→ARM64EC call → regression risk on working
  plugins (BIAS/TONEX) **UNVERIFIED** — must regression-test before trusting it.
- It only **reveals the next wall**: with both fixes, PACE init throws ~1086 handled
  null-read exceptions → `c0000026 EXCEPTION_INVALID_DISPOSITION` → **stack overflow**
  (stack 0x20000-0x120000). Another FEX-fidelity gap in PACE's obfuscated code.

## Dead-end experiments (documented so nobody repeats them)
- **0xCC stack pre-fill** (`virtual.c` VSTPOC_STACK_FILL_CC) — FAILED. The region PACE
  scans is live caller-frame data, overwritten before the scan; an allocation-time
  fill can't reach it.
- **`signal_arm64ec.c` `is_valid_arm64ec_frame` diagnostic** — only a logging aid; the
  "invalid frame" was a secondary unwinding artifact, not the real crash.
- **dnssd.dll** — the LM dynamically `LoadLibrary("dnssd.dll")`s (Apple Bonjour) and
  hard-crashes if absent. `patches/dnssd_stub.c` is a 6-export stub (x86_64) that
  satisfies it; needed for the LM but only relevant once PACE runtime works.

## Debug harness (KEPT in mainline — reusable for ANY plugin)
`app/src/full/.../debug/DebugRunReceiver.kt` (full-flavor manifest, gated on
`BuildConfig.DEBUG`) forks `wine <exe>` against a prefix from the app process (SELinux
needs the app-domain fork, not `adb run-as`). No UI needed:
```
adb shell am broadcast -n com.varcain.guitarrackcraft/com.varcain.guitarrackcraft.debug.DebugRunReceiver \
  -a com.varcain.guitarrackcraft.DEBUG_RUN \
  --es exe "/data/user/0/com.varcain.guitarrackcraft/files/wineprefix_<X>/drive_c/.../LDSvc.exe" \
  --es prefix "/data/user/0/com.varcain.guitarrackcraft/files/wineprefix_<X>"
```
- Args via `cache/exe_args.txt`; WINEDEBUG/env via `cache/wine_env.txt`; log →
  `cache/vst_host_installer.log`.
- LDSvc.exe = `…/Common Files/PACE/Services/LicenseServices/LDSvc.exe -u https://activation.paceap.com/InitiateActivation`.
- A crashed run leaves a **zombie** LDSvc.exe + sets `g_installer`; `adb shell
  am force-stop <pkg>` before re-firing, then relaunch the app so symlink-races re-pin.

## How to resume
1. `git apply` both PARKED patches (FEX + wine rpcrt4) to the submodules.
2. Build: `libarm64ecfex.dll` (ninja in `external/fex-upstream/build-arm64ec`,
   target `Bin/libarm64ecfex.dll`) and `rpcrt4.dll` (the aarch64-windows make target).
   ⚠ `build-arm64ec/Bin` had a 5.1MB build ≠ the 4.8MB shipped — rebuild stock FEX
   first or you ship the wrong base (see memory feedback_wine_patch_repack_traps).
3. Hot-deploy with the symlink-race trick (cache copy + 0.1s `ln -sf;mv` loop beating
   applyManifestSymlinks) — libwine_* mapping: fex=f5eb, rpcrt4=f1aa, msi64=f134,
   msi32=f421, wineboot=f2f4, unix-ntdll=f00e.
4. Reproduce via the harness (LDSvc.exe), then attack the **next wall**: the
   null-read exception loop / stack overflow in PACE init.
5. Open priorities: (a) harden the FEX x4 offset so it's layout-robust; (b)
   regression-test the FEX change on BIAS/TONEX; (c) file the ExitFunctionEC fix
   upstream with FEX (AndreRH asked for it).

## References
- Hangover #225: https://github.com/AndreRH/hangover/issues/225 (+ commit cc6dbd8)
- MS ARM64EC ABI (variadic x4/x5): https://learn.microsoft.com/en-us/windows/arm/arm64ec-abi
- iLok WoA/emulation unsupported (PACE ARM64 beta, full rollout "through 2026"):
  https://help.ilok.com/faq_ilm.html
- Memory notes: `feedback_ilok_pace_fex_arm64ec`, `feedback_ilok_sha256_processor_arch`
