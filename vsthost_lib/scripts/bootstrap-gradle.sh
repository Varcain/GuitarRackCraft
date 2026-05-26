#!/usr/bin/env bash
# Bootstraps the Gradle wrapper (jar + gradlew + gradlew.bat) by downloading
# a Gradle distribution and using its `gradle wrapper` task once.
# Run this once after cloning. After that, use ./gradlew normally.
#
# Requires: curl, unzip, a JDK (17+) on PATH.
set -euo pipefail

GRADLE_VERSION="${GRADLE_VERSION:-8.7}"
DIST_URL="https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

echo "Downloading Gradle ${GRADLE_VERSION}..."
curl -fL --progress-bar -o "$TMPDIR/gradle.zip" "$DIST_URL"

echo "Extracting..."
unzip -q "$TMPDIR/gradle.zip" -d "$TMPDIR"
GRADLE_HOME="$(find "$TMPDIR" -maxdepth 1 -type d -name "gradle-${GRADLE_VERSION}*" | head -1)"
if [[ -z "$GRADLE_HOME" ]]; then
    echo "Failed to locate extracted gradle distribution." >&2
    exit 1
fi

echo "Generating wrapper in $REPO_ROOT..."
cd "$REPO_ROOT"
"$GRADLE_HOME/bin/gradle" --no-daemon wrapper --gradle-version "$GRADLE_VERSION"

echo
echo "Wrapper installed."
echo "Build:    ./gradlew assembleDebug"
echo "Install:  ./gradlew installDebug   (USB device connected via adb)"
