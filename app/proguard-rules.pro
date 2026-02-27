# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep JNI bridge classes (PluginInfo, PortInfo, ScalePoint constructed via JNI reflection)
-keep class com.varcain.guitarrackcraft.engine.** { *; }

# Keep activities referenced in AndroidManifest
-keep class com.varcain.guitarrackcraft.MainActivity { *; }
-keep class com.varcain.guitarrackcraft.X11PluginUIActivity { *; }

# Keep Kotlin enums (used in when-expressions, serialization)
-keepclassmembers enum * {
    public static **[] values();
    public static ** valueOf(java.lang.String);
}

# Keep Compose runtime classes
-keep class androidx.compose.** { *; }
-dontwarn androidx.compose.**
