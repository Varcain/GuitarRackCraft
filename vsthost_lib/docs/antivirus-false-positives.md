# Antivirus false positives on the full (Windows-VST) APK

## What was flagged, and why

The GitHub release `v0.1-experimental-vst-2` (`app-full-release.apk`) was
flagged by **15 of 75** VirusTotal engines (`vst-1`: 11). VirusTotal's own
Android sandbox rated it clean — the detections are purely static.

VirusTotal's per-file data for the APK showed exactly one kind of culprit:

| Files inside the APK VirusTotal scanned | Flagged |
|---|---|
| ELF (`.so`) | 0 of 105 |
| dex | 0 of 2 |
| Windows PE DLL | 44 of 72 |
| Windows PE EXE | 7 of 10 |

Every flagged file was a **wine builtin PE binary**, and the packaging made
them look as suspicious as possible:

- all ~1.5k wine DLLs/EXEs (i386 + ARM64X) were shipped **disguised as
  `lib/arm64-v8a/libwine_NNNN.so`** (file-type masquerading);
- **nothing was pruned** — `regedit`, `mshta`, `powershell`, `certutil`,
  `dxtrans`, the printing stack, … (worst: `dxtrans.dll` 28 engines,
  `mfh264enc.dll` 15, `regedit.exe` 10);
- builds weren't reproducible, so every release carried new, unknown hashes
  and any reputation or whitelisting was lost.

The APK-level verdicts were mostly Win32 families (`Gen:Variant.Zusy`,
`W32/PossibleThreat`, `Trojan.Ulise`, `W32/ABTrojan`, `Mal/FakeAV`) plus three
Android reputation/risk scores (Avast-Mobile `RepMalware`, Symantec
`AppRisk:Generisk`, Trustlook).

## What changed

| Change | Where |
|---|---|
| PE files ship under their real names in `assets/wine/…` and are extracted at first run; only wine's ELF side stays in `lib/` (descriptive `libwine_*.so` names). Works because the full flavor targets SDK 28 (SELinux grants execute + execmod on app files). | `scripts/pack-wine-fex.py`, `WineRuntimeManifest.kt`, `WineAssetInstaller.kt` |
| 335 unneeded PE files pruned (programs allowlist + subsystem denylist), guarded by an import/forwarder check that fails the pack if anything kept depends on something pruned. | `wine-prune.conf`, `scripts/wine_prune.py` |
| Reproducible PE builds: no build timestamps (`/Brepro`, `--no-insert-timestamp`), no checkout paths (`-ffile-prefix-map`). | `scripts/build-*.sh` |
| Unused hook libs dropped; debug receiver debug-only + DUMP-protected; `VstEditorActivity` not exported. | packer, manifests |

## Result

VirusTotal, release `vst-2` vs a test build with all of the above
(2026-09-28, `vt_check.py vt-diff`):

| | `v0.1-experimental-vst-2` | after |
|---|---|---|
| Engines flagging the APK | 15 / 75 | **7 / 75** |
| Flagged files among those VirusTotal extracted | 51 of 190 | **0 of 100** |

Stopped flagging: Avast-Mobile, Symantec Mobile Insight, Trustlook (the
Android reputation/risk scores), Fortinet, Varist, K7 (×2), CAT-QuickHeal.
Still flagging: **BitDefender and its OEMs** Arcabit, Emsisoft, GData,
VIPRE (one `Gen:Variant.Yogi` / `Babar` family — a single report to
BitDefender addresses 5 of the 7), CTX, ZoneAlarm.

## Checking a build

```
python3 tools/vt_check.py audit app-full-release.apk          # offline: PE inventory, fails on PE under lib/
VT_API_KEY_FILE=~/.config/vt/api_key \
  python3 tools/vt_check.py vt app-full-release.apk --upload --wait --children --json new.json
python3 tools/vt_check.py vt-diff old.json new.json            # engines + inner files, before/after
```

Keep the VirusTotal key in a mode-600 file (`VT_API_KEY_FILE`), not in
argv or shell history. Uploading makes the APK available to VirusTotal's
partners — fine for a release build, think twice for unreleased ones.

Reproducibility check: build twice (wiped build dirs) and compare
`assets/wine-fex-manifest.json` — every entry's `sha256` and the
`pack_digest` must match; `vt_check.py audit --baseline` on the two APKs
must report no added/removed PE files.

## Reporting the remaining detections to vendors

Some detections on core wine modules can't be engineered away (e.g. `atl.dll`,
`msdmo.dll`, single-engine ML hits on `combase`, `cmd`, `wineboot`): they
are unsigned re-implementations of Windows system DLLs. Those need vendor
false-positive reports — and **only stick while hashes stay stable**, which
is what the reproducible builds are for. Submit against a release build
whose PE hashes you intend to keep shipping.

Current contacts / portals: VirusTotal's
[False Positive Contacts](https://docs.virustotal.com/docs/false-positive-contacts)
list (kept up to date there, so not duplicated here).

Suggested order (most detections removed per report first):

1. **BitDefender** — its engine also powers Arcabit, eScan, Emsisoft,
   GData, VIPRE and ALYac, so one fix clears ~6 verdicts.
2. **Avast / AVG** (Gen Digital; one submission covers both, plus
   Avast-Mobile).
3. **Microsoft** (WDSI, "software developer" submission).
4. **Symantec / Broadcom** (also Symantec Mobile Insight).
5. **Trend Micro**, **Sophos**.
6. The rest as they appear: K7, Fortinet, Varist, CAT-QuickHeal, Cynet,
   Elastic, Palo Alto, DeepInstinct, Trapmine, MaxSecure, Avira, Ikarus,
   Trellix/Skyhigh, Google, Kingsoft, Bkav, Malwarebytes, Antiy, Lionic,
   CTX, Trustlook.

### Submission template

> **Product:** Guitar RackCraft — open-source (GPL-3.0) Android guitar
> effects app, <https://github.com/Varcain/GuitarRackCraft>, release
> `<tag>` (`app-full-release.apk`, sha256 `<sha256>`).
>
> **Detection:** `<engine>` reports `<detection name>` on the APK /
> on `<inner file>` (sha256 `<sha256>`).
>
> **Why it is a false positive:** the file is a builtin DLL/EXE of
> [Wine](https://www.winehq.org/) (LGPL), rebuilt from source
> (`vsthost_lib/external/wine-upstream`, patches in
> `vsthost_lib/patches/wine`) so the app can host Windows VST plugins on
> Android. It is not a Windows binary of ours and does nothing on its own.
> The build is reproducible: the same source produces byte-identical
> files, and the hashes stay the same across app releases unless wine
> itself changes.
>
> **Attached / linked:** VirusTotal report link, list of affected inner
> files with sha256.

### Tracking

| Vendor | Detection(s) | Submitted (date, ticket) | Status |
|---|---|---|---|
| BitDefender | | | |
| Avast / AVG | | | |
| Microsoft | | | |
| Symantec | | | |
| Trend Micro | | | |
| Sophos | | | |

After a vendor fixes its signature, request a re-scan on VirusTotal
(re-analyse button, or `POST /api/v3/files/{sha256}/analyse`) and re-run
`vt-diff` to confirm.
