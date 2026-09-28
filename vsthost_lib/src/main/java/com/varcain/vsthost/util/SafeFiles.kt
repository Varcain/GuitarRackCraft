package com.varcain.vsthost.util

import java.io.File
import java.io.IOException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.attribute.BasicFileAttributes

/**
 * Recursively delete this file/directory WITHOUT following symlinks.
 *
 * Use this instead of Kotlin's [File.deleteRecursively] for anything that can
 * contain a wine prefix: that walker descends through directory symlinks, and
 * every prefix has `dosdevices/z: -> /` (plus `c: -> ../drive_c`), so deleting a
 * prefix with it walks the whole device filesystem, deleting whatever the app
 * can write and looping through `/proc/self/root`.
 *
 * Here a symlink (at any depth, including this file itself) is unlinked, never
 * entered. Best effort: entries that can't be deleted are skipped and the walk
 * continues.
 *
 * @return true if the tree no longer exists afterwards.
 */
fun File.deleteTreeNoFollow(): Boolean {
    val root = toPath()
    if (!Files.exists(root, LinkOption.NOFOLLOW_LINKS)) return true
    // walkFileTree without FileVisitOption.FOLLOW_LINKS reports a symlink via
    // visitFile (with isSymbolicLink attrs) and never descends into its target.
    Files.walkFileTree(root, object : SimpleFileVisitor<Path>() {
        override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
            runCatching { Files.delete(file) }
            return FileVisitResult.CONTINUE
        }

        override fun visitFileFailed(file: Path, exc: IOException): FileVisitResult {
            runCatching { Files.delete(file) }
            return FileVisitResult.CONTINUE
        }

        override fun postVisitDirectory(dir: Path, exc: IOException?): FileVisitResult {
            runCatching { Files.delete(dir) }
            return FileVisitResult.CONTINUE
        }
    })
    return !Files.exists(root, LinkOption.NOFOLLOW_LINKS)
}
