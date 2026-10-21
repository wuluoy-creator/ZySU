package com.zying.zysu.ui.sumh

import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.IOException
import kotlin.test.*

class SUMHDiagnosticPackageStorageTest {
    @get:Rule val temporary = TemporaryFolder()
    private val pending = SUMHDiagnosticPackagePending("create", "a".repeat(32), null)
    @Test fun checkpointContainsOnlyRecoveryIdentityAndIsVersioned() {
        for (value in listOf(null, pending, SUMHDiagnosticPackagePending("remove", "b".repeat(32), "sha256-v1:" + "c".repeat(64)))) {
            val encoded = encodeSUMHDiagnosticPackagePending(value)
            assertTrue(encoded.size <= SUMH_DIAGNOSTIC_CHECKPOINT_BYTES)
            assertEquals(value, decodeSUMHDiagnosticPackagePending(encoded))
            assertFails { decodeSUMHDiagnosticPackagePending(encoded + 0) }
            assertFails { decodeSUMHDiagnosticPackagePending(encoded.copyOf().also { it[0] = 0 }) }
        }
        assertFails { SUMHDiagnosticPackagePending("create", pending.id, "sha256-v1:" + "c".repeat(64)) }
        assertFails { SUMHDiagnosticPackagePending("remove", pending.id, null) }
    }
    @Test fun writeSyncsParentAndDirectoryAndClearsByDurableEmptyRecord() {
        val directory = File(temporary.root, "diagnostic-packages"); val syncs = mutableListOf<File>()
        val storage = SUMHDiagnosticPackageFile(directory) { syncs += it }
        assertNull(storage.read()); assertFalse(directory.exists()); assertTrue(syncs.isEmpty())
        assertTrue(storage.compareAndSet(null, pending)); assertEquals(pending, storage.read()); assertEquals(listOf(temporary.root, directory), syncs)
        assertEquals(listOf("pending-v1.bin"), directory.list()!!.toList())
        assertTrue(storage.compareAndSet(pending, null)); assertNull(storage.read()); assertTrue(File(directory, "pending-v1.bin").isFile)
    }
    @Test fun futureCorruptAndOversizedInputArePreserved() {
        val directory = temporary.newFolder("diagnostic-packages"); val file = File(directory, "pending-v1.bin")
        val storage = SUMHDiagnosticPackageFile(directory) { }
        for (bad in listOf(byteArrayOf(0, 1, 2), ByteArray(257), encodeSUMHDiagnosticPackagePending(pending).also { it[3]++ })) {
            file.writeBytes(bad); assertFails { storage.read() }; assertFails { storage.compareAndSet(null, null) }; assertContentEquals(bad, file.readBytes())
        }
    }
    @Test fun postRenameSyncFailureLeavesQueryableIdentityAndDoesNotClaimSuccess() {
        val directory = File(temporary.root, "diagnostic-packages")
        val storage = SUMHDiagnosticPackageFile(directory) { if (it == directory) throw IOException("unconfirmed") }
        assertFails { storage.compareAndSet(null, pending) }
        assertEquals(pending, storage.read()); assertFalse(File(directory, "pending-v1.stage").exists())
    }
    @Test fun unsafeStageAndDirectoryDoNotReplaceCheckpoint() {
        val directory = temporary.newFolder("diagnostic-packages")
        val storage = SUMHDiagnosticPackageFile(directory) { }; assertTrue(storage.compareAndSet(null, pending))
        val before = File(directory, "pending-v1.bin").readBytes()
        File(directory, "pending-v1.stage").mkdir()
        assertFails { storage.compareAndSet(pending, null) }; assertContentEquals(before, File(directory, "pending-v1.bin").readBytes())
        val other = temporary.newFile("wrong-directory")
        assertFails { SUMHDiagnosticPackageFile(other) { }.read() }
    }
    @Test fun staleEmptyOwnerCannotReplaceAnotherOwnersDurableClaim() {
        val directory = File(temporary.root, "diagnostic-packages")
        val first = SUMHDiagnosticPackageFile(directory) { }
        val second = SUMHDiagnosticPackageFile(directory) { }
        val firstObserved = first.read(); val secondObserved = second.read()
        val other = SUMHDiagnosticPackagePending("create", "b".repeat(32), null)
        assertTrue(first.compareAndSet(firstObserved, pending))
        val bytes = File(directory, "pending-v1.bin").readBytes()
        assertFalse(second.compareAndSet(secondObserved, other))
        assertEquals(pending, second.read()); assertContentEquals(bytes, File(directory, "pending-v1.bin").readBytes())
    }
    @Test fun staleAcknowledgementCannotClearNewClaimOrAnotherRevision() {
        val directory = File(temporary.root, "diagnostic-packages")
        val first = SUMHDiagnosticPackageFile(directory) { }
        val second = SUMHDiagnosticPackageFile(directory) { }
        assertTrue(first.compareAndSet(null, pending))
        val stale = second.read()
        assertTrue(first.compareAndSet(pending, null))
        val replacement = SUMHDiagnosticPackagePending("remove", pending.id, "sha256-v1:" + "c".repeat(64))
        assertTrue(first.compareAndSet(null, replacement))
        val bytes = File(directory, "pending-v1.bin").readBytes()
        assertFalse(second.compareAndSet(stale, null))
        assertFalse(second.compareAndSet(replacement.copy(revision = "sha256-v1:" + "d".repeat(64)), null))
        assertEquals(replacement, second.read()); assertContentEquals(bytes, File(directory, "pending-v1.bin").readBytes())
    }
}
