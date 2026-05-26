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

plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.varcain.vsthost"
    compileSdk = 35
    ndkVersion = "26.1.10909125"

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
            // Extract libvsthost.so + libhello_x86_64.so + libwine_*.so to
            // nativeLibraryDir so they can be execve'd / mmap-execed (the
            // only path Android lets untrusted apps run from).
            useLegacyPackaging = true
            // The libwine_*.so set is Wine's binaries renamed to lib*.so so
            // AGP packages them. Most are x86_64 ELF or PE (not aarch64), so
            // aarch64-linux-android-strip would fail. Keep them as-is.
            keepDebugSymbols += listOf(
                "*/arm64-v8a/libwine_*.so",
                "*/arm64-v8a/libhello_x86_64.so",
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
