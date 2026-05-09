package com.deviceagent

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ShouldReportProgressTest {
    @Test
    fun firstReportAlwaysTriggers() {
        assertTrue(shouldReportProgress(0L, 0L, 1_000L, 1L, 100L))
    }

    @Test
    fun thirtySecondsTriggersEvenWithSmallByteDelta() {
        assertTrue(shouldReportProgress(1_000L, 1_000L, 31_000L, 1_100L, 100L * MB))
    }

    @Test
    fun fivePercentTriggers() {
        assertTrue(shouldReportProgress(1_000L, 0L, 2_000L, 6L * MB, 100L * MB))
    }

    @Test
    fun oneMbFloorAppliesWhenFivePercentIsSmaller() {
        assertFalse(shouldReportProgress(1_000L, 0L, 2_000L, 600L * KB, 10L * MB))
        assertTrue(shouldReportProgress(1_000L, 0L, 2_000L, 1_100L * KB, 10L * MB))
    }

    @Test
    fun belowTimeAndByteThresholdDoesNotTrigger() {
        assertFalse(shouldReportProgress(1_000L, 0L, 6_000L, 100L * KB, 100L * MB))
    }

    @Test
    fun unknownTotalOnlyTriggersByTime() {
        assertFalse(shouldReportProgress(1_000L, 0L, 6_000L, 100L * MB, -1L))
        assertTrue(shouldReportProgress(1_000L, 0L, 31_000L, 100L * MB, -1L))
    }

    private companion object {
        const val KB = 1024L
        const val MB = 1024L * 1024
    }
}
