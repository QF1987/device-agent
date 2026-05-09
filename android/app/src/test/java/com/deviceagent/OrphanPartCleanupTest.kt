package com.deviceagent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.concurrent.TimeUnit

class OrphanPartCleanupTest {
    @Test
    fun deletesOnlyPartFilesOlderThanSevenDays() {
        val dir = Files.createTempDirectory("orphan-part-cleanup-test-").toFile()
        val now = 1_000_000_000_000L
        val old = File(dir, "old.apk.part").apply { writeText("old"); setLastModified(now - TimeUnit.DAYS.toMillis(8)) }
        val fresh = File(dir, "fresh.apk.part").apply { writeText("fresh"); setLastModified(now - TimeUnit.DAYS.toMillis(1)) }
        val ancient = File(dir, "ancient.apk.part").apply { writeText("ancient"); setLastModified(now - TimeUnit.DAYS.toMillis(30)) }
        val nonPart = File(dir, "keep.apk").apply { writeText("keep"); setLastModified(now - TimeUnit.DAYS.toMillis(30)) }

        val result = cleanupOrphanPartFiles(dir, now)

        assertEquals(OrphanPartCleanupResult(scanned = 3, deleted = 2, kept = 1), result)
        assertFalse(old.exists())
        assertTrue(fresh.exists())
        assertFalse(ancient.exists())
        assertTrue(nonPart.exists())
    }
}
