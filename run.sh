#!/bin/bash

# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

# Unified build-and-run script for Guitar RackCraft.
#
# Usage:
#   ./run.sh              # debug (default)
#   ./run.sh debug        # full (VST) flavor, debug build
#   ./run.sh release      # full (VST) flavor, release build + AAB
#   ./run.sh playstore    # playstore flavor (no VST), release AAB + bundletool install
#
# The `full` flavor builds the Windows-VST host stack (wine/FEX/DXVK/Mesa) from
# source via build.sh — the first build is slow (~60-90 min), incremental after.
# Set BUILD_VST=0 to skip it and build a VST-less `full` APK.

set -e

MODE="${1:-debug}"

# ── Common setup ──────────────────────────────────────────────────────────────

# Prefer a system JDK 17 (AGP can fail with Java 21 on the jlink step); fall
# back to Android Studio's bundled JBR, which is often the only JDK on a dev
# box. Respects an already-exported JAVA_HOME.
if [ -z "${JAVA_HOME:-}" ]; then
    for jdk in \
        /usr/lib/jvm/java-17-openjdk-amd64 /usr/lib/jvm/java-17-openjdk \
        /usr/lib/jvm/jdk-17 /usr/lib/jvm/java-17 \
        "$HOME"/tools/android-studio/*/jbr "$HOME"/android-studio/jbr \
        /opt/android-studio/jbr /usr/local/android-studio/jbr; do
        if [ -x "$jdk/bin/java" ]; then
            export JAVA_HOME="$jdk"
            break
        fi
    done
fi

export ANDROID_HOME=~/Android/Sdk
export ANDROID_SDK_ROOT=~/Android/Sdk
export PATH=$PATH:$ANDROID_HOME/tools:$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

check_device() {
    local count
    count=$(adb devices | grep -v "List of devices" | grep "device$" | wc -l)
    echo "$count"
}

# Upload keystore — loaded from .env (not committed)
ENV_FILE="$PROJECT_ROOT/.env"
if [ -f "$ENV_FILE" ]; then
    set -a; source "$ENV_FILE"; set +a
fi

# ── Mode dispatch ─────────────────────────────────────────────────────────────

case "$MODE" in

# ══════════════════════════════════════════════════════════════════════════════
# DEBUG
# ══════════════════════════════════════════════════════════════════════════════
debug)
    echo "Guitar RackCraft - Debug Build"
    echo "========================="
    echo ""

    echo "Running native build..."
    ./build.sh
    echo ""

    echo "Building debug APK..."
    ./gradlew assembleFullDebug
    echo ""

    if [ "$(check_device)" -eq 0 ]; then
        echo "No device connected."
        echo "APK: app/build/outputs/apk/full/debug/app-full-debug.apk"
        exit 1
    fi

    echo "Installing..."
    ./gradlew installFullDebug
    echo ""

    echo "Starting app..."
    adb shell am start -n com.varcain.guitarrackcraft/.MainActivity
    echo ""
    echo "Debug build installed and started."
    ;;

# ══════════════════════════════════════════════════════════════════════════════
# RELEASE
# ══════════════════════════════════════════════════════════════════════════════
release)
    echo "Guitar RackCraft - Release Build"
    echo "==========================="
    echo ""

    echo "Running native build..."
    ./build.sh
    echo ""

    echo "Building release AAB + APK..."
    ./gradlew bundleFullRelease assembleFullRelease
    echo ""

    AAB=app/build/outputs/bundle/fullRelease/app-full-release.aab
    if [ -f "$AAB" ]; then
        echo "AAB: $AAB ($(du -sh "$AAB" | cut -f1))"
    fi

    APK=app/build/outputs/apk/full/release/app-full-release.apk
    if [ -f "$APK" ]; then
        echo "APK: $APK ($(du -sh "$APK" | cut -f1))"
    fi
    echo ""

    if [ "$(check_device)" -eq 0 ]; then
        echo "No device connected — skipping install."
        exit 0
    fi

    if [ -f "$APK" ]; then
        echo "Installing release APK..."
        adb install -r "$APK"
    else
        echo "No signed APK found — skipping install."
        exit 0
    fi
    echo ""

    echo "Starting app..."
    adb shell am start -n com.varcain.guitarrackcraft/.MainActivity
    echo ""
    echo "Release build installed and started."
    ;;

# ══════════════════════════════════════════════════════════════════════════════
# PLAYSTORE
# ══════════════════════════════════════════════════════════════════════════════
playstore)
    echo "Guitar RackCraft - Play Store Build"
    echo "=============================="
    echo ""

    # Ensure bundletool
    BUNDLETOOL="$PROJECT_ROOT/bundletool.jar"
    if [ ! -f "$BUNDLETOOL" ]; then
        BUNDLETOOL_VERSION="1.17.2"
        echo "Downloading bundletool $BUNDLETOOL_VERSION..."
        curl -sL "https://github.com/google/bundletool/releases/download/${BUNDLETOOL_VERSION}/bundletool-all-${BUNDLETOOL_VERSION}.jar" \
            -o "$BUNDLETOOL"
        echo ""
    fi

    echo "Running native build (playstore)..."
    ./build.sh playstore
    echo ""

    echo "Building playstore AAB..."
    ./gradlew bundlePlaystoreRelease
    echo ""

    AAB=$(find app/build/outputs/bundle/playstoreRelease -name '*.aab' 2>/dev/null | head -1)
    if [ -z "$AAB" ]; then
        echo "ERROR: AAB not found"
        exit 1
    fi
    echo "AAB: $AAB ($(du -sh "$AAB" | cut -f1))"
    echo "  Signed with upload key — ready for Google Play."
    echo ""

    if [ "$(check_device)" -eq 0 ]; then
        echo "No device connected."
        echo ""
        echo "To install manually:"
        echo "  java -jar bundletool.jar build-apks \\"
        echo "    --bundle=$AAB --output=out.apks --local-testing \\"
        echo "    --ks=$UPLOAD_KS --ks-pass=pass:$UPLOAD_KS_PASS \\"
        echo "    --ks-key-alias=$UPLOAD_KEY_ALIAS --key-pass=pass:$UPLOAD_KEY_PASS"
        echo "  java -jar bundletool.jar install-apks --apks=out.apks"
        exit 1
    fi

    # Generate split APKs with --local-testing for PAD simulation
    APKS="$PROJECT_ROOT/build/playstore-local.apks"
    mkdir -p "$(dirname "$APKS")"
    rm -f "$APKS"

    echo "Generating split APKs (local-testing PAD simulation)..."
    java -jar "$BUNDLETOOL" build-apks \
        --bundle="$AAB" \
        --output="$APKS" \
        --local-testing \
        --ks="$UPLOAD_KS" \
        --ks-pass=pass:"$UPLOAD_KS_PASS" \
        --ks-key-alias="$UPLOAD_KEY_ALIAS" \
        --key-pass=pass:"$UPLOAD_KEY_PASS"
    echo ""

    echo "Installing on device..."
    java -jar "$BUNDLETOOL" install-apks --apks="$APKS"
    echo ""

    echo "Starting app..."
    adb shell am start -n com.varcain.guitarrackcraft/.MainActivity
    echo ""
    echo "Play Store build installed and started."
    echo "Install-time asset packs delivered via --local-testing mode."
    ;;

*)
    echo "Usage: $0 [debug|release|playstore]"
    exit 1
    ;;
esac

echo ""
echo "Logs: adb logcat | grep -E 'AudioEngine|NativeBridge|LV2Plugin|PluginBrowser'"
