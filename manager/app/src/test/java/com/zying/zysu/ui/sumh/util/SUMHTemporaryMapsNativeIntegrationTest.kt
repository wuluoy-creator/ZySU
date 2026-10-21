package com.zying.zysu.ui.sumh.util

import kotlinx.coroutines.test.runTest
import org.junit.Test
import kotlin.test.*

/** Fixtures come from the production C++ callback/client and transport envelope serializer. */
class SUMHTemporaryMapsNativeIntegrationTest {
    private fun fixture(name: String): ByteArray = requireNotNull(javaClass.getResourceAsStream(
        "/sumh-temporary-maps-native/$name-envelope.json")).use { it.readBytes() }

    @Test fun nativeCapabilitiesPreserveIndependentBitsAndFailedReads() {
        val supported = SUMHTemporaryMapsParser.capabilities(fixture("capabilities-supported"))
        assertEquals(SUMHTemporaryMapsSupport.Supported, supported.inventory)
        assertEquals(SUMHTemporaryMapsSupport.Supported, supported.delete)
        val inventory = SUMHTemporaryMapsParser.capabilities(fixture("capabilities-inventory-only"))
        assertEquals(SUMHTemporaryMapsSupport.Supported, inventory.inventory)
        assertEquals(SUMHTemporaryMapsSupport.Unsupported, inventory.delete)
        for (name in listOf("capabilities-unsupported", "capabilities-delete-only")) {
            val result = SUMHTemporaryMapsParser.capabilities(fixture(name))
            assertEquals(SUMHTemporaryMapsSupport.Unsupported, result.inventory)
            assertEquals(SUMHTemporaryMapsSupport.Unsupported, result.delete)
        }
        for (errno in listOf(4, 5, 13, 25)) {
            val result = SUMHTemporaryMapsParser.capabilities(fixture("capabilities-error-$errno"))
            assertEquals(SUMHTemporaryMapsSupport.Unknown, result.inventory)
            assertEquals(SUMHTemporaryMapsSupport.Unknown, result.delete)
            assertEquals(errno, result.errno)
        }
        val malformed = SUMHTemporaryMapsParser.capabilities(fixture("capabilities-malformed-negative"))
        assertEquals(SUMHTemporaryMapsSupport.Unknown, malformed.inventory)
        assertEquals(SUMHTemporaryMapsSupport.Unknown, malformed.delete)
    }

    @Test fun nativePagesPreserveUnsignedPrecisionBytesAndPartialState() {
        val complete = appendSUMHTemporaryMapsPage(null, SUMHTemporaryMapsParser.page(fixture("page-high-u64")))
        assertTrue(complete.complete)
        assertEquals("18446744073709551615", complete.token.instance)
        assertEquals("9007199254740993", complete.token.generation)
        val row = complete.rows.single()
        assertEquals("18446744073709551615", row.targetIno)
        assertEquals("4294967295", row.targetDev)
        assertEquals("9007199254740993", row.spoofedIno)
        assertEquals("/synthetic/$() ` ", row.spoofedPath.utf8)
        val invalid = appendSUMHTemporaryMapsPage(null, SUMHTemporaryMapsParser.page(fixture("page-invalid-utf8")))
        assertEquals("2f61ff", invalid.rows.single().spoofedPath.hex)
        assertNull(invalid.rows.single().spoofedPath.utf8)
        val partial = appendSUMHTemporaryMapsPage(null, SUMHTemporaryMapsParser.page(fixture("page-partial")))
        assertFalse(partial.complete)
        assertEquals("1", partial.nextCursor)
        for ((name, expected) in listOf("page-unsupported" to SUMHInventoryPageStatus.Unsupported,
            "page-stale" to SUMHInventoryPageStatus.Stale)) {
            val result = SUMHTemporaryMapsParser.page(fixture(name))
            assertEquals(expected, result.page?.status)
            assertFailsWith<IllegalArgumentException> { appendSUMHTemporaryMapsPage(null, result) }
        }
    }

    private val request get() = SUMHTemporaryMapsRemoveRequest(
        SUMHTemporaryMapsToken("18446744073709551615", "9007199254740993", 127),
        SUMHTemporaryMapsRow("1", "0", "0", "7", "0", SUMHInventoryBytes("2f61", "/a")))

    @Test fun nativeReceiptsKeepAcknowledgementRejectionAndUnknownDistinct() {
        for ((name, status) in listOf(
            "delete-acknowledged" to SUMHTemporaryMapsRemoveStatus.Deleted,
            "delete-stale" to SUMHTemporaryMapsRemoveStatus.NotApplied,
            "delete-missing" to SUMHTemporaryMapsRemoveStatus.NotApplied,
            "delete-unsupported" to SUMHTemporaryMapsRemoveStatus.NotApplied,
            "delete-unknown-4" to SUMHTemporaryMapsRemoveStatus.Unknown,
            "delete-unknown-5" to SUMHTemporaryMapsRemoveStatus.Unknown)) {
            val result = SUMHTemporaryMapsParser.remove(fixture(name), request)
            assertEquals(status, result.status)
            assertEquals(status == SUMHTemporaryMapsRemoveStatus.Deleted, result.acknowledged)
            assertEquals(name != "delete-unsupported", result.attempted)
        }
    }

    @Test fun nativeAcknowledgementCannotAuthorizeAnotherKeyOrTriggerReplay() = runTest {
        var calls = 0
        val service = NativeSUMHTemporaryMapsService {
            calls++
            fixture("delete-acknowledged")
        }
        val changed = request.copy(row = request.row.copy(targetIno = "1"))
        assertEquals(SUMHTemporaryMapsRemoveStatus.Unknown, service.remove(changed).status)
        assertEquals(1, calls)
    }
}
