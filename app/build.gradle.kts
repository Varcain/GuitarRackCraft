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

import java.text.SimpleDateFormat
import java.util.Date
import java.net.InetAddress

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.varcain.guitarrackcraft"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.varcain.guitarrackcraft"
        minSdk = 26
        targetSdk = 35
        versionCode = 100  // 0.1
        versionName = "0.1-main"

        val buildDate = SimpleDateFormat("yyyy-MM-dd").format(Date())
        val buildTime = SimpleDateFormat("HH:mm").format(Date())
        val buildHost = System.getenv("HOSTNAME")
            ?: System.getenv("COMPUTERNAME")
            ?: try { InetAddress.getLocalHost().hostName } catch (_: Exception) { "unknown" }
        buildConfigField("String", "BUILD_DATE", "\"$buildDate\"")
        buildConfigField("String", "BUILD_TIME", "\"$buildTime\"")
        buildConfigField("String", "BUILD_HOST", "\"$buildHost\"")

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        vectorDrawables {
            useSupportLibrary = true
        }

        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }

        ndk {
            // LV2 libs (lilv, etc.) are built for arm64-v8a only; use arm64-v8a until armeabi-v7a libs are built
            abiFilters += listOf("arm64-v8a")
        }
    }

    signingConfigs {
        create("release") {
            val ksFile = findProperty("RELEASE_STORE_FILE")?.toString().orEmpty()
            val ksPass = findProperty("RELEASE_STORE_PASSWORD")?.toString().orEmpty()
            val kAlias = findProperty("RELEASE_KEY_ALIAS")?.toString().orEmpty()
            val kPass  = findProperty("RELEASE_KEY_PASSWORD")?.toString().orEmpty()
            if (ksFile.isNotEmpty()) {
                storeFile = file(ksFile)
                storePassword = ksPass
                keyAlias = kAlias
                keyPassword = kPass
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
    }

    flavorDimensions += "distribution"
    productFlavors {
        create("full") {
            dimension = "distribution"
            buildConfigField("boolean", "USE_ASSET_PACKS", "false")
        }
        create("playstore") {
            dimension = "distribution"
            buildConfigField("boolean", "USE_ASSET_PACKS", "true")
        }
    }

    assetPacks += listOf(":gxplugins_pack", ":neural_pack", ":brummer_pack")

    lint {
        checkReleaseBuilds = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    buildFeatures {
        compose = true
        buildConfig = true
    }

    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.4"
    }

    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
        jniLibs {
            useLegacyPackaging = true
            // build.sh renames SONAME-versioned libs (libxcb.so.1 -> libxcb.so) so Android
            // extracts them to nativeLibDir. Only liblilv-0.so.0 keeps its versioned name.
            pickFirsts += listOf("**/liblilv-0.so.0")
        }
    }

    // Copy liblilv-0.so.0 (versioned SONAME) to merged/stripped native libs
    afterEvaluate {
        val jniDir = file("src/main/jniLibs/arm64-v8a")
        val lv2LibDir = file("src/main/cpp/libs/lv2/lib")

        fun copyExtraNativeLibs(targetDir: java.io.File) {
            if (!targetDir.exists()) return
            // liblilv-0.so.0 needs special handling: versioned SONAME file + unversioned alias
            var liblilvSo0 = file("$jniDir/liblilv-0.so.0")
            if (!liblilvSo0.exists()) liblilvSo0 = file("$lv2LibDir/liblilv-0.so.0")
            if (liblilvSo0.exists()) {
                copy { from(liblilvSo0); into(targetDir) }
                copy { from(liblilvSo0); into(targetDir); rename { "liblilv-0.so" } }
                println("Copied liblilv-0.so.0 (+ alias liblilv-0.so) to ${targetDir.absolutePath}")
            }
        }

        for (flavor in listOf("Full", "Playstore")) {
            for (buildType in listOf("Debug", "Release")) {
                val variant = "$flavor$buildType"
                val varLower = variant.replaceFirstChar { it.lowercase() }
                tasks.findByName("merge${variant}NativeLibs")?.doLast {
                    copyExtraNativeLibs(file("build/intermediates/merged_native_libs/$varLower/out/lib/arm64-v8a"))
                }
                tasks.findByName("strip${variant}DebugSymbols")?.doLast {
                    copyExtraNativeLibs(file("build/intermediates/stripped_native_libs/$varLower/out/lib/arm64-v8a"))
                }
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    sourceSets {
        getByName("main") {
            assets.srcDirs("src/main/assets")
        }
    }
}

dependencies {
    // X11 plugin UIs: native EGL + ANativeWindow (see app/src/main/cpp/x11/)

    // Core Android
    implementation("androidx.core:core-ktx:1.12.0")
    implementation("androidx.lifecycle:lifecycle-runtime-ktx:2.6.2")
    implementation("androidx.activity:activity-compose:1.8.1")

    // Compose
    implementation(platform("androidx.compose:compose-bom:2023.10.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-graphics")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-extended")

    // ViewModel
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.6.2")

    // Navigation
    implementation("androidx.navigation:navigation-compose:2.7.5")

    // WebView safe file access (avoid file:// access denied on API 29+)
    implementation("androidx.webkit:webkit:1.8.0")

    // Permissions
    implementation("com.google.accompanist:accompanist-permissions:0.32.0")

    // Networking & JSON
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
    implementation("com.google.code.gson:gson:2.10.1")

    // Image loading
    implementation("io.coil-kt:coil-compose:2.5.0")

    // Testing
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test:rules:1.5.0")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.5.1")
    androidTestImplementation("androidx.test.uiautomator:uiautomator:2.3.0")
    androidTestImplementation(platform("androidx.compose:compose-bom:2023.10.01"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-tooling")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
