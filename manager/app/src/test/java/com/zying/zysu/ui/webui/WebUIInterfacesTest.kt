package com.zying.zysu.ui.webui

import android.app.Activity
import android.app.Application
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.util.Base64
import android.webkit.WebView
import com.zying.zysu.ui.viewmodel.SuperUserViewModel.AppInfo
import com.zying.zysu.ui.webui.file.KsuIO
import com.zying.zysu.ui.webui.file.ManagedInputStream
import com.zying.zysu.ui.webui.pm.KsuPm
import org.json.JSONArray
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.Robolectric
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import kotlin.test.assertContentEquals
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

@RunWith(RobolectricTestRunner::class)
@Config(application = Application::class, sdk = [35])
class WebUIInterfacesTest {
    private fun app(name: String, uid: Int, system: Boolean = false): AppInfo {
        val info = PackageInfo().apply {
            packageName = name
            versionName = "1.0"
            longVersionCode = 42
            applicationInfo = ApplicationInfo().apply {
                this.uid = uid
                this.packageName = name
                flags = if (system) ApplicationInfo.FLAG_SYSTEM else 0
            }
        }
        return AppInfo(name, info, null)
    }

    @Test fun packageQueriesPreserveAllUsersAndMissingEntries() {
        val pm = KsuPm { listOf(app("app.a", 1012345), app("app.a", 12345), app("sys.b", 1000, true)) }
        assertEquals("[\"app.a\"]", pm.listPackages("user"))
        assertEquals("[\"sys.b\"]", pm.listPackages("system"))
        val result = JSONArray(pm.getPackagesInfo("[\"app.a\",\"missing\"]"))
        assertEquals(3, result.length())
        assertEquals(12345, result.getJSONObject(0).getInt("uid"))
        assertEquals(0, result.getJSONObject(0).getInt("userId"))
        assertEquals(1012345, result.getJSONObject(1).getInt("uid"))
        assertEquals(10, result.getJSONObject(1).getInt("userId"))
        assertEquals(42, result.getJSONObject(1).getInt("versionCode"))
        assertEquals("missing", result.getJSONObject(2).getString("packageName"))
        assertTrue(result.getJSONObject(2).has("error"))
    }

    @Test fun closingOneWebUiDoesNotCloseAnotherWebUiOrAcceptNewStreams() {
        val first = KsuIO()
        val second = KsuIO()
        var closed = false
        first.openOutputStreams["first"] = object : ByteArrayOutputStream() {
            override fun close() { closed = true; super.close() }
        }
        val survivor = ByteArrayOutputStream()
        second.openOutputStreams["second"] = survivor
        first.destroy()
        assertTrue(closed)
        assertTrue(first.openOutputStreams.isEmpty())
        assertFalse(first.canOpenStream())
        assertEquals("", first.FileInputStream().open("/never/opened"))
        assertEquals("", first.FileOutputStream().open("/never/opened"))
        assertEquals("", first.RandomAccessFile().open("/never/opened", "rw"))
        assertTrue(second.FileOutputStream().write("second", "aGVsbG8="))
        assertContentEquals("hello".toByteArray(), survivor.toByteArray())
        assertFalse(first.FileOutputStream().write("second", "aA=="))
        second.destroy()
    }

    @Test fun inputStreamsReadInOrderAndClampOversizedChunks() {
        val io = KsuIO()
        val bytes = ByteArray(100000) { (it % 251).toByte() }
        io.openInputStreams["data"] = ManagedInputStream(ByteArrayInputStream(bytes))
        val reader = io.FileInputStream()
        val first = Base64.decode(reader.read("data", Int.MAX_VALUE), Base64.DEFAULT)
        val second = Base64.decode(reader.read("data", Int.MAX_VALUE), Base64.DEFAULT)
        assertEquals(64 * 1024, first.size)
        assertContentEquals(bytes, first + second)
        assertEquals("", reader.read("data"))
        assertTrue(reader.close("data"))
        assertFalse(reader.close("data"))
        io.destroy()
    }

    @Test fun blobSessionsAreIsolatedAndCleanedOnFailureAndDestroy() {
        val controller = Robolectric.buildActivity(Activity::class.java).setup()
        val activity = controller.get()
        val view = WebView(activity)
        val first = WebUIDownloadInterface(activity, view)
        val second = WebUIDownloadInterface(activity, view)
        val before = activity.cacheDir.listFiles().orEmpty().map { it.name }.toSet()
        try {
            val id = first.startChunkedDownload("../report.txt", "text/plain")
            val other = second.startChunkedDownload("other.txt", "text/plain")
            assertTrue(id.isNotBlank())
            assertFalse(second.writeDownloadChunk(id, "aA=="))
            assertTrue(first.writeDownloadChunk(id, "aGVsbG8="))
            first.destroy()
            assertFalse(first.writeDownloadChunk(id, "aA=="))
            assertEquals("", first.startChunkedDownload("new.txt", null))
            assertTrue(second.writeDownloadChunk(other, "aA=="))
            assertFalse(second.writeDownloadChunk(other, "A".repeat(1024 * 1024 + 1)))
            assertFalse(second.completeChunkedDownload(other))
        } finally {
            first.destroy()
            second.destroy()
            view.destroy()
            controller.pause().stop().destroy()
        }
        assertEquals(before, activity.cacheDir.listFiles().orEmpty().map { it.name }.toSet())
    }

    @Test fun downloadNamesStayWithinDownloadsAndMimeIsPreserved() {
        assertEquals("report.txt", sanitizeWebUIDownloadFileName("../../report.txt"))
        assertEquals("report.txt", sanitizeWebUIDownloadFileName("C:\\temp\\report.txt"))
        assertEquals("download.bin", sanitizeWebUIDownloadFileName(".."))
        assertEquals("ab.txt", sanitizeWebUIDownloadFileName("a\n\u0000b.txt"))
        assertEquals("application/json", resolveWebUIDownloadMimeType("file.bin", "application/json; charset=utf-8"))
        assertEquals("text/plain", resolveWebUIDownloadMimeType("report.txt", "application/octet-stream"))
    }
}
