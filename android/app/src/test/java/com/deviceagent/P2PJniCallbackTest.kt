package com.deviceagent

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Test
import java.io.File

class P2PJniCallbackTest {
    @Test
    fun exposesExpectedP2PJniCallbackSignatures() {
        assertNotNull(
            DeviceAgentService::class.java.getDeclaredMethod(
                "onP2PStarted",
                String::class.java,
                String::class.java,
                String::class.java,
                String::class.java,
                String::class.java
            )
        )
        assertNotNull(
            DeviceAgentService::class.java.getDeclaredMethod(
                "onP2PProgress",
                Int::class.javaPrimitiveType,
                Long::class.javaPrimitiveType,
                Long::class.javaPrimitiveType
            )
        )
        assertNotNull(
            DeviceAgentService::class.java.getDeclaredMethod(
                "onP2PComplete",
                Boolean::class.javaPrimitiveType,
                String::class.java
            )
        )
    }

    @Test
    fun p2pContextCarriesInstallInputs() {
        val ctx = DeviceAgentService.P2PDownloadContext(
            batchId = "batch-p2p",
            fileId = "file-p2p",
            commandId = "cmd-p2p",
            sha256 = "abc123",
            localPath = "/data/data/com.deviceagent/files/downloads/file-p2p.apk"
        )

        assertEquals("batch-p2p", ctx.batchId)
        assertEquals("file-p2p", ctx.fileId)
        assertEquals("cmd-p2p", ctx.commandId)
        assertEquals("abc123", ctx.sha256)
        assertEquals("file-p2p.apk", File(ctx.localPath).name)
    }

    @Test
    fun handleDownloadAndInstallAcceptsPredownloadedApkHook() {
        val method = DeviceAgentService::class.java.getDeclaredMethod(
            "handleDownloadAndInstall",
            String::class.java,
            String::class.java,
            String::class.java,
            String::class.java,
            String::class.java,
            File::class.java
        )
        assertNotNull(method)
    }
}
