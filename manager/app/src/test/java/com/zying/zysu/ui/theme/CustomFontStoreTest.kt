package com.zying.zysu.ui.theme

import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.content.SharedPreferences
import androidx.compose.ui.text.font.FontFamily
import kotlinx.coroutines.CancellationException
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStream
import java.lang.reflect.Proxy
import kotlin.test.*

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class CustomFontStoreTest {
    private val context: Context get() = RuntimeEnvironment.getApplication()
    private val prefs get() = context.getSharedPreferences("appearance", Context.MODE_PRIVATE)
    private val folder get() = File(context.filesDir, "custom-font")
    private fun font() = checkNotNull(javaClass.getResourceAsStream("/fixtures/custom-font/LiberationSans-Regular.ttf"))
    private fun selectedFile() = File(folder, checkNotNull(prefs.getString("custom_font_file", null)))

    @Before fun reset() = AppearancePreferences.reset(context)

    @Test fun validatedFontPersistsAndAppliesToEveryTypographyRole() {
        val imported = CustomFontStore.import(context, "Liberation Sans.ttf", font())
        assertNotNull(imported.typeface)
        AppearancePreferences.applyCustomFont(imported)
        assertEquals("custom", AppearancePreferences.fontFamily)
        AppearancePreferences.load(context)
        assertEquals("Liberation Sans.ttf", AppearancePreferences.customFontName)
        assertNotNull(AppearancePreferences.customFontFamily)
        assertNull(AppearancePreferences.customFontProblem)
        val custom = AppearancePreferences.customFontFamily
        val typography = Typography.withAppearance(1.2f, "custom", custom)
        listOf(typography.displayLarge, typography.headlineMedium, typography.titleSmall,
            typography.bodyLarge, typography.labelSmall).forEach { assertEquals(custom, it.fontFamily) }
        assertEquals(Typography.bodyLarge.fontSize.value * 1.2f, typography.bodyLarge.fontSize.value)
        assertEquals(FontFamily.Default, Typography.withAppearance(1f, "custom").bodyLarge.fontFamily)
    }

    @Test fun invalidAndUnsupportedFilesPreservePreviousValidFont() {
        CustomFontStore.import(context, "Working.ttf", font())
        val previous = selectedFile()
        val bytes = previous.readBytes()
        listOf(byteArrayOf(), "not a font".toByteArray(), "ttcf-not-supported".toByteArray()).forEach { input ->
            val error = assertFailsWith<CustomFontException> {
                CustomFontStore.import(context, "Broken.otf", ByteArrayInputStream(input))
            }
            assertEquals(CustomFontProblem.INVALID, error.problem)
            assertEquals(previous, selectedFile())
            assertContentEquals(bytes, previous.readBytes())
            assertNotNull(CustomFontStore.load(context).typeface)
        }
        assertTrue(folder.listFiles().orEmpty().none { it.name.startsWith("pending-") })
    }

    @Test fun importStopsAtSizeLimitAndClosesStreamWithoutReplacingFont() {
        CustomFontStore.import(context, "Working.ttf", font())
        val previous = selectedFile()
        var closed = false
        val oversized = object : InputStream() {
            var remaining = CustomFontStore.MAX_BYTES.toLong() + 1
            override fun read(): Int = if (remaining-- > 0) 0 else -1
            override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
                if (remaining <= 0) return -1
                val count = minOf(length.toLong(), remaining).toInt()
                buffer.fill(0, offset, offset + count)
                remaining -= count
                return count
            }
            override fun close() { closed = true }
        }
        assertEquals(CustomFontProblem.TOO_LARGE, assertFailsWith<CustomFontException> {
            CustomFontStore.import(context, "Oversized.ttf", oversized)
        }.problem)
        assertTrue(closed)
        assertEquals(previous, selectedFile())
        assertTrue(folder.listFiles().orEmpty().none { it.name.startsWith("pending-") })
    }

    @Test fun cancellationDuringCopyRetainsPreviousFontAndRemovesStaging() {
        CustomFontStore.import(context, "Working.ttf", font())
        val previous = selectedFile()
        var checkpoints = 0
        assertFailsWith<CancellationException> {
            CustomFontStore.import(context, "Cancelled.ttf", font()) {
                if (++checkpoints == 3) throw CancellationException("screen left")
            }
        }
        assertEquals(previous, selectedFile())
        assertEquals("Working.ttf", CustomFontStore.load(context).name)
        assertTrue(folder.listFiles().orEmpty().none { it.name.startsWith("pending-") })
    }

    @Test fun replacementPublishesNewFileAndRemovalOnlyDeletesOwnedFile() {
        CustomFontStore.import(context, "First.ttf", font())
        val first = selectedFile()
        val unrelated = File(folder, "notes.txt").apply { writeText("keep") }
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "Second.otf", font()))
        assertFalse(first.exists())
        val second = selectedFile()
        assertNotEquals(first, second)
        assertTrue(second.exists())
        AppearancePreferences.setFontFamily(context, "serif")
        AppearancePreferences.removeCustomFont(context)
        assertFalse(second.exists())
        assertEquals("keep", unrelated.readText())
        assertEquals("serif", AppearancePreferences.fontFamily)
        assertNull(AppearancePreferences.customFontName)
    }

    @Test fun missingOrCorruptSelectedFontFallsBackWithoutCrashing() {
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "First.ttf", font()))
        selectedFile().delete()
        AppearancePreferences.load(context)
        assertEquals("system", AppearancePreferences.fontFamily)
        assertEquals(CustomFontProblem.MISSING, AppearancePreferences.customFontProblem)
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "Second.ttf", font()))
        selectedFile().writeText("corrupted")
        AppearancePreferences.load(context)
        assertEquals("system", AppearancePreferences.fontFamily)
        assertEquals(CustomFontProblem.CORRUPT, AppearancePreferences.customFontProblem)
    }

    @Test fun tamperedStoredPathCannotReadOrDeleteOutsideOwnedDirectory() {
        val unrelated = File(context.filesDir, "preserved.ttf").apply { writeText("keep") }
        prefs.edit().putString("custom_font_file", "../preserved.ttf").putString("custom_font_name", "Invalid")
            .putString("font_family", "custom").commit()
        AppearancePreferences.load(context)
        assertEquals("system", AppearancePreferences.fontFamily)
        assertEquals(CustomFontProblem.MISSING, AppearancePreferences.customFontProblem)
        AppearancePreferences.removeCustomFont(context)
        assertEquals("keep", unrelated.readText())
    }

    @Test fun failedPersistenceRestoresFontAndAppearanceWithoutCrashingReset() {
        AppearancePreferences.applyCustomFont(CustomFontStore.import(context, "Preserved.ttf", font()))
        AppearancePreferences.setFontScale(context, 1.2f)
        AppearancePreferences.setAmoled(context, true)
        val original = selectedFile()
        assertFailsWith<CustomFontException> { CustomFontStore.import(failingCommitContext(), "Failed.ttf", font()) }
        assertEquals(original, selectedFile())
        assertFailsWith<CustomFontException> { AppearancePreferences.removeCustomFont(failingCommitContext()) }
        assertEquals(original, selectedFile())
        var reported = false
        AppearancePreferences.reset(failingCommitContext()) { reported = true }
        assertTrue(reported)
        assertTrue(original.isFile)
        AppearancePreferences.load(context)
        assertEquals("custom", AppearancePreferences.fontFamily)
        assertEquals("Preserved.ttf", AppearancePreferences.customFontName)
        assertEquals(1.2f, AppearancePreferences.fontScale)
        assertTrue(AppearancePreferences.amoled)
    }

    /** Android changes its memory map before commit reports an IO failure. Simulate that boundary. */
    private fun failingCommitContext(): Context {
        val backing = prefs
        var failNext = true
        val wrapped = object : SharedPreferences by backing {
            override fun edit(): SharedPreferences.Editor {
                val real = backing.edit()
                lateinit var proxy: SharedPreferences.Editor
                proxy = Proxy.newProxyInstance(SharedPreferences.Editor::class.java.classLoader,
                    arrayOf(SharedPreferences.Editor::class.java)) { _, method, args ->
                    val result = method.invoke(real, *(args ?: emptyArray()))
                    when {
                        method.name == "commit" && failNext -> { failNext = false; false }
                        result === real -> proxy
                        else -> result
                    }
                } as SharedPreferences.Editor
                return proxy
            }
        }
        return object : ContextWrapper(context) {
            override fun getSharedPreferences(name: String, mode: Int) =
                if (name == "appearance") wrapped else super.getSharedPreferences(name, mode)
        }
    }
}
