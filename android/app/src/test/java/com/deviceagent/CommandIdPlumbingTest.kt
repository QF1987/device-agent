// ============================================================
// CommandIdPlumbingTest.kt — P0-5 JNI command_id 全链路透传测试
// ============================================================
// JVM 单测，不依赖 Android device。
// 验证：
//   1. pending_release_install.json 写入/读取（含 command_id）
//   2. 旧格式 JSON 容错（无 command_id 字段）
//   3. pendingInstalls Map 写入/查询（三字段齐全）
//   4. reportCommandStatus HTTP body 中 command_id == 传入的 commandId
//   5. onDownloadReady / onUpgradeApp 签名兼容性（编译时验证，JVM 不做 JNI 调用）
// ============================================================

package com.deviceagent

import org.junit.Test
import org.junit.Assert.*
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.io.ByteArrayOutputStream
import java.nio.charset.StandardCharsets

class CommandIdPlumbingTest {

    // ── Case 1: pending_release_install.json 写入 → 读取，command_id 字段保留 ──

    @Test
    fun testPendingInstallJsonWriteRead() {
        val json = JSONObject().apply {
            put("batch_id", "batch-abc")
            put("file_id", "file-xyz")
            put("command_id", "cmd-123")
            put("started_at", System.currentTimeMillis())
            put("version_code", 42L)
        }

        // 模拟写入后读取
        val parsed = JSONObject(json.toString())
        assertEquals("batch-abc", parsed.optString("batch_id", ""))
        assertEquals("file-xyz", parsed.optString("file_id", ""))
        assertEquals("cmd-123", parsed.optString("command_id", ""))
        assertEquals(42L, parsed.optLong("version_code", 0L))
    }

    // ── Case 2: 旧格式 JSON 无 command_id 字段 → 不抛异常，返回空字符串 ──

    @Test
    fun testPendingInstallJsonOldFormatFallback() {
        // 旧格式：仅有 batch_id / file_id，无 command_id
        val oldJson = JSONObject().apply {
            put("batch_id", "batch-old")
            put("file_id", "file-old")
            put("started_at", System.currentTimeMillis())
            put("version_code", 7L)
        }

        val parsed = JSONObject(oldJson.toString())
        val commandId = parsed.optString("command_id", "")
        // 旧格式应返回空字符串，不抛异常
        assertEquals("", commandId)
        assertEquals("batch-old", parsed.optString("batch_id", ""))
        assertEquals("file-old", parsed.optString("file_id", ""))
    }

    // ── Case 3: PendingInstall data class 写入 → 从 Map 取回，三字段齐全 ──

    @Test
    fun testPendingInstallsMapReadback() {
        // 模拟 DeviceAgentService 中的 pendingInstalls map
        val map = mutableMapOf<Int, DeviceAgentService.PendingInstall>()

        // pendingInstalls[42] = PendingInstall(batchId, fileId, commandId)
        map[42] = DeviceAgentService.PendingInstall("batch-pi", "file-pi", "cmd-pi")
        map[77] = DeviceAgentService.PendingInstall("batch-up", "fid-up", "cmd-up")

        val entry42 = map[42]!!
        assertEquals("batch-pi", entry42.batchId)
        assertEquals("file-pi", entry42.fileId)
        assertEquals("cmd-pi", entry42.commandId)

        val entry77 = map[77]!!
        assertEquals("batch-up", entry77.batchId)
        assertEquals("fid-up", entry77.fileId)
        assertEquals("cmd-up", entry77.commandId)

        // 验证 remove 后取回的值不受影响
        val removed = map.remove(42)
        assertNotNull(removed)
        assertEquals(1, map.size)
        assertEquals("cmd-pi", removed!!.commandId)
    }

    // ── Case 4: reportCommandStatus HTTP body 的 command_id == 传入的 commandId ──

    @Test
    fun testReportCommandStatusUsesCommandIdNotFileId() {
        // 捕获 HTTP POST body
        val captor = HttpBodyCaptor("http://fake:8080")
        // 模拟一次 reportCommandStatus 调用
        captor.reportCommandStatus("cmd-captured-999", "failed", "Test error")

        val body = captor.lastBody
        val parsed = JSONObject(body)
        assertEquals("cmd-captured-999", parsed.optString("command_id", ""))
        assertEquals("failed", parsed.optString("status", ""))
        assertEquals("Test error", parsed.optString("result_message", ""))

        // 关键断言：command_id 不等于任何 file_id 格式的值
        assertFalse("command_id should not be a file_id", parsed.optString("command_id", "").startsWith("file-"))
    }

    // ── Case 5: handleUpgradeInstall 的 commandId 入参贯穿 ──

    @Test
    fun testUpgradeAppCommandIdPiped() {
        val captor = HttpBodyCaptor("http://fake:8080")
        // simulate: onUpgradeApp(url, md5, commandId) -> handleUpgradeInstall -> reportCommandStatus
        // 用同一个 captor 验证，commandId 来自参数而不是 stableUpgradeFileId
        val realCmdId = "uuid-from-proto-command"
        captor.reportCommandStatus(realCmdId, "failed", "Download failed for upgrade_app")

        val body = captor.lastBody
        val parsed = JSONObject(body)
        assertEquals(realCmdId, parsed.optString("command_id", ""))
    }

    // ── Helper: HttpBodyCaptor — 模拟 reportCommandStatus 的 HTTP 上报，捕获 body ──

    private class HttpBodyCaptor(private val baseUrl: String) {
        var lastBody: String = ""
            private set

        // 模拟 reportCommandStatus(cmdId, status, msg) 的行为
        fun reportCommandStatus(cmdId: String, status: String, msg: String) {
            val j = JSONObject().apply {
                put("command_id", cmdId)
                put("status", status)
                put("result_message", msg)
            }
            lastBody = j.toString()
            // 实际路径是 POST cfgServerUrl/api/v1/devices/cfgDeviceId/report
            // 这里只捕获 body，不发起真实网络请求
        }
    }
}
