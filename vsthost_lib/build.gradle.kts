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

import java.util.Properties

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

// Pinned toolchain versions, shared with :app and the native prebuild.
val toolchain = Properties().apply {
    rootProject.file("config/toolchain.properties").inputStream().use { load(it) }
}

android {
    namespace = "com.varcain.vsthost"
    compileSdk = 35
    // :app's NDK: libvsthost.so runs on the libc++_shared.so :app packages
    // (and hands it a std::unique_ptr, VstFactory.h).
    ndkVersion = toolchain.getProperty("ndk.version")

    defaultConfig {
        // Matches GuitarRackCraft :app minSdk. vstpoc historically used 27;
        // dropping to 26 to align with consumer. If runtime needs an API 27+
        // symbol, surface via Build.VERSION.SDK_INT guards rather than raising
        // the floor.
        minSdk = 26
        // Library-level marker only — the consumer's per-flavor targetSdk
        // controls actual runtime behavior. In GuitarRackCraft, only the
        // `full` flavor (targetSdk=28) depends on this lib; the `playstore`
        // flavor (targetSdk=35) does not, because wine's PE relocations need
        // pre-Android-10 SELinux execmod that's denied at targetSdk >= 29.
        // See vstpoc memory: feedback_targetsdk35_blocked.md
        targetSdk = 28

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                cppFlags += listOf("-std=c++20", "-fvisibility=hidden")
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
        buildConfig = true
        // Expose libvsthost.so + selected headers to :app via prefab so the
        // app's CMakeLists can find_package(vsthost_lib) and call into
        // vsthost::createVstFactory(...). Per-flavor: only :app's `full`
        // variant consumes this (fullImplementation in app/build.gradle.kts).
        prefabPublishing = true
    }

    buildTypes {
        debug {
            // The AhbSpike/AhbChannelTest developer diagnostics (see
            // src/main/cpp/CMakeLists.txt) - left out of release builds.
            externalNativeBuild {
                cmake {
                    arguments += "-DVSTHOST_DEBUG_DIAGNOSTICS=ON"
                }
            }
        }
    }

    prefab {
        create("vsthost") {
            headers = "src/main/cpp"
        }
    }

    composeOptions {
        // Matches :app and GuitarRackCraft's Kotlin 1.9.20.
        kotlinCompilerExtensionVersion = "1.5.4"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    packaging {
        jniLibs {
            // Extract libvsthost.so + libwine_*.so to nativeLibraryDir so
            // the wine loader/wineserver can be execve'd (the only place
            // targetSdk>=29-style W^X lets untrusted apps exec from).
            useLegacyPackaging = true
            // libwine_*.so are wine's ELF binaries (loader, wineserver,
            // aarch64-unix libs) renamed to lib*.so so AGP packages them;
            // pack-wine-fex.py already strips them. The PE side ships in
            // assets (see pack-wine-fex.py / WineAssetInstaller).
            keepDebugSymbols += listOf(
                "*/arm64-v8a/libwine_*.so",
            )
        }
    }
}

dependencies {
    // Align with :app — Compose BOM 2023.10.01, Kotlin 1.9.20 era.
    val composeBom = platform("androidx.compose:compose-bom:2023.10.01")
    implementation(composeBom)

    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.6.2")
    implementation("androidx.activity:activity-compose:1.8.1")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
}
