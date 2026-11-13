package com.zying.zysu.ui.theme

import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.content.SharedPreferences
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.net.Uri
import kotlinx.coroutines.runBlocking
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class HomeCardArtworkStoreTest {
    private val context: Context get() = RuntimeEnvironment.getApplication()
    @Before fun reset() = runBlocking { HomeCardArtworkStore.clear(context); Unit }

    private fun image(name: String, transparent: Boolean = true): File {
        val bitmap = Bitmap.createBitmap(40, 30, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(if (transparent) Color.TRANSPARENT else Color.CYAN)
        bitmap.setPixel(20, 15, Color.RED)
        return File(context.cacheDir, name).also { file ->
            file.outputStream().use { assertTrue(bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) }
            bitmap.recycle()
        }
    }

    @Test fun transparentImageRemainsPrivateAndKeepsItsOriginalAlpha() = runBlocking {
        val source = image("transparent.png")
        val bytes = source.readBytes()
        assertEquals(ArtworkImportResult.Success, HomeCardArtworkStore.import(context, Uri.fromFile(source)),
            org.robolectric.shadows.ShadowLog.getLogsForTag("HomeCardArtwork").joinToString { it.throwable?.stackTraceToString() ?: it.msg })
        source.delete()
        val stored = assertNotNull(HomeCardArtworkStore.read(context).file)
        assertContentEquals(bytes, stored.readBytes())
        val bitmap = BitmapFactory.decodeFile(stored.absolutePath)
        assertEquals(0, Color.alpha(bitmap.getPixel(0, 0)))
        assertEquals(Color.RED, bitmap.getPixel(20, 15))
        bitmap.recycle()
    }

    @Test fun replacementAndRemovalOnlyDeleteOwnedImage() = runBlocking {
        val source = image("old.png")
        HomeCardArtworkStore.import(context, Uri.fromFile(source))
        val old = assertNotNull(HomeCardArtworkStore.read(context).file)
        val replacement = image("new.png", false)
        assertEquals(ArtworkImportResult.Success, HomeCardArtworkStore.import(context, Uri.fromFile(replacement)))
        val current = assertNotNull(HomeCardArtworkStore.read(context).file)
        assertNotEquals(old, current)
        assertFalse(old.exists())
        assertTrue(HomeCardArtworkStore.clear(context))
        assertFalse(current.exists())
        assertTrue(source.exists())
        assertTrue(replacement.exists())
        assertNull(HomeCardArtworkStore.read(context).file)
    }

    @Test fun malformedAndOversizedImportsPreservePreviousImageAndTransform() = runBlocking {
        HomeCardArtworkStore.import(context, Uri.fromFile(image("valid.png")))
        HomeCardArtworkStore.setScale(context, 1.75f)
        HomeCardArtworkStore.setCrop(context, true)
        val previous = HomeCardArtworkStore.read(context)
        val bad = File(context.cacheDir, "not-an-image.png").apply { writeText("not an image") }
        assertEquals(ArtworkImportResult.InvalidImage, HomeCardArtworkStore.import(context, Uri.fromFile(bad)))
        val large = File(context.cacheDir, "too-large.png")
        java.io.RandomAccessFile(large, "rw").use { it.setLength(HomeCardArtworkStore.MAX_BYTES + 1L) }
        assertEquals(ArtworkImportResult.TooLarge, HomeCardArtworkStore.import(context, Uri.fromFile(large)))
        assertEquals(previous, HomeCardArtworkStore.read(context))
        assertTrue(previous.file!!.exists())
        assertFalse(previous.file.parentFile!!.listFiles().orEmpty().any { it.name.startsWith("pending-") })
    }

    @Test fun scaleModeAndMissingFileFallbackSurviveNewReads() {
        HomeCardArtworkStore.setScale(context, 2.25f)
        HomeCardArtworkStore.setCrop(context, true)
        assertEquals(2.25f, HomeCardArtworkStore.read(context).scale)
        assertTrue(HomeCardArtworkStore.read(context).crop)
        HomeCardArtworkStore.setScale(context, Float.NaN)
        assertEquals(1f, HomeCardArtworkStore.read(context).scale)
        HomeCardArtworkStore.setScale(context, 900f)
        assertEquals(3f, HomeCardArtworkStore.read(context).scale)
        HomeCardArtworkStore.preferences(context).edit().putString("filename", "../outside.png").commit()
        assertNull(HomeCardArtworkStore.read(context).file)
        HomeCardArtworkStore.preferences(context).edit().putString("filename", "artwork-deadbeef.img").commit()
        assertNull(HomeCardArtworkStore.read(context).file)
    }

    @Test fun failedCommitRestoresSelectionForReplacementAndRemoval() = runBlocking {
        HomeCardArtworkStore.import(context, Uri.fromFile(image("working.png")))
        HomeCardArtworkStore.setScale(context, 1.8f)
        HomeCardArtworkStore.setCrop(context, true)
        val previous = HomeCardArtworkStore.read(context)
        val failingContext = FailedCommitContext(context)
        failingContext.failNext = true
        assertEquals(ArtworkImportResult.Failed,
            HomeCardArtworkStore.import(failingContext, Uri.fromFile(image("replacement.png", false))))
        assertEquals(previous, HomeCardArtworkStore.read(context))
        assertTrue(previous.file!!.exists())
        failingContext.failNext = true
        assertFalse(HomeCardArtworkStore.clear(failingContext))
        assertEquals(previous, HomeCardArtworkStore.read(context))
        assertTrue(previous.file.exists())
    }

    /** Model Android's disk-failure contract: the memory map changes even when commit returns false. */
    private class FailedCommitContext(base: Context) : ContextWrapper(base) {
        var failNext = false
        override fun getSharedPreferences(name: String, mode: Int): SharedPreferences {
            val original = super.getSharedPreferences(name, mode)
            return object : SharedPreferences by original {
                override fun edit(): SharedPreferences.Editor {
                    val delegate = original.edit()
                    return object : SharedPreferences.Editor by delegate {
                        override fun putString(key: String, value: String?) = apply { delegate.putString(key, value) }
                        override fun putFloat(key: String, value: Float) = apply { delegate.putFloat(key, value) }
                        override fun putBoolean(key: String, value: Boolean) = apply { delegate.putBoolean(key, value) }
                        override fun clear() = apply { delegate.clear() }
                        override fun commit(): Boolean {
                            val success = delegate.commit()
                            if (failNext) { failNext = false; return false }
                            return success
                        }
                    }
                }
            }
        }
    }
}
