package com.varcain.vsthost.wine

import android.content.Context
import org.json.JSONObject

/**
 * The wine runtime as described by assets/wine-fex-manifest.json (written by
 * scripts/pack-wine-fex.py).
 *
 *  - schema 1 (legacy): every entry is a lib*.so in nativeLibraryDir that gets
 *    symlinked to its wine path — including the PE DLLs/EXEs, which the packer
 *    disguised as libwine_NNNN.so.
 *  - schema 2: only the ELF side (wine loader, wineserver, aarch64-unix/…so)
 *    lives in nativeLibraryDir ("elf" entries). PE files ship under their real
 *    names in APK assets ("asset" entries) and are extracted into the wine root
 *    as read-only regular files — see [WineAssetInstaller].
 */
internal class WineRuntimeManifest(
    val schema: Int,
    /** schema 2: digest over all entries; changes whenever any file changes. */
    val packDigest: String?,
    val elf: List<ElfEntry>,
    val assets: List<AssetEntry>,
) {
    /** Symlink `<wineRoot>/[relPath]` → `<nativeLibraryDir>/[lib]`. */
    data class ElfEntry(val relPath: String, val lib: String)

    /** Extract `assets/[asset]` to `<wineRoot>/[relPath]`; [sha256] is lowercase hex. */
    data class AssetEntry(val relPath: String, val asset: String, val size: Long, val sha256: String)

    companion object {
        const val ASSET_NAME = "wine-fex-manifest.json"

        fun load(ctx: Context): WineRuntimeManifest =
            parse(ctx.assets.open(ASSET_NAME).bufferedReader().use { it.readText() })

        fun parse(json: String): WineRuntimeManifest {
            val m = JSONObject(json)
            val schema = m.optInt("schema", 1)
            require(schema == 1 || schema == 2) { "unsupported wine manifest schema $schema" }
            val root = m.getString("wine_root_device")  // "/wine"
            val elf = mutableListOf<ElfEntry>()
            val assets = mutableListOf<AssetEntry>()
            val entries = m.getJSONArray("entries")
            for (i in 0 until entries.length()) {
                val e = entries.getJSONObject(i)
                val path = e.getString("path")  // e.g. /wine/bin/wine
                require(path.startsWith("$root/")) { "manifest path $path outside expected root $root" }
                // Paths under wineRoot are relative — strip the leading /wine/.
                val rel = path.removePrefix("$root/")
                require(rel.split('/').none { it.isEmpty() || it == "." || it == ".." }) {
                    "bad manifest path $path"
                }
                when (if (schema == 1) "elf" else e.getString("kind")) {
                    "elf" -> elf += ElfEntry(rel, e.getString("lib"))
                    "asset" -> assets += AssetEntry(
                        rel, e.getString("asset"), e.getLong("size"), e.getString("sha256").lowercase())
                    else -> throw IllegalArgumentException("unknown manifest entry kind: $e")
                }
            }
            return WineRuntimeManifest(schema, m.optString("pack_digest").ifEmpty { null }, elf, assets)
        }
    }
}
