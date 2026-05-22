// ============================================================
// service/DeviceAgentService.kt - Android Service
// ============================================================
package com.deviceagent

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.SystemClock
import android.widget.Toast
import com.deviceagent.install.IAppInstaller
import com.deviceagent.install.InstallEvent
import com.deviceagent.install.InstallEventBus
import com.deviceagent.install.SilentInstaller
import com.deviceagent.install.CustomInstaller
import com.deviceagent.install.NormalInstaller
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.lang.ref.WeakReference
import java.util.concurrent.TimeUnit
import org.json.JSONObject



private const val PREFS_NAME = "device_agent_config"
private const val KEY_SERVER_URL = "server_url"
private const val KEY_DEVICE_ID = "device_id"
private const val PENDING_INSTALL_FILE = "pending_release_install.json"
private var cfgServerUrl: String = ""
private var cfgDeviceId: String = ""
private var mainServiceRef: WeakReference<DeviceAgentService>? = null

private const val PROGRESS_REPORT_INTERVAL_MS = 30_000L
private const val PROGRESS_MIN_BYTES = 1L * 1024 * 1024
private const val ACTION_VIEW_INSTALL_TIMEOUT_MS = 2 * 60 * 1000L
private const val TRANSIENT_NOTIFICATION_MS = 10_000L
private const val IDLE_NOTIFICATION_TEXT = "device-agent running"

internal fun shouldReportProgress(
    lastReportTimeMs: Long,
    lastReportBytes: Long,
    nowMs: Long,
    absBytes: Long,
    totalBytes: Long
): Boolean {
    if (lastReportTimeMs == 0L) return true
    if (nowMs - lastReportTimeMs >= PROGRESS_REPORT_INTERVAL_MS) return true
    if (totalBytes > 0) {
        val threshold = maxOf(totalBytes / 20, PROGRESS_MIN_BYTES)
        if (absBytes - lastReportBytes >= threshold) return true
    }
    return false
}

internal fun buildDownloadText(absBytes: Long, totalBytes: Long): String {
    val absMb = maxOf(absBytes, 0L) / (1024 * 1024)
    if (totalBytes <= 0) return "⏬ 下载 ${absMb}MB"
    val totalMb = maxOf(totalBytes, 0L) / (1024 * 1024)
    val pct = ((absBytes * 100) / totalBytes).coerceIn(0, 100)
    return "⏬ 下载 ${pct}% (${absMb}/${totalMb}MB)"
}

internal data class OrphanPartCleanupResult(val scanned: Int, val deleted: Int, val kept: Int)

internal fun cleanupOrphanPartFiles(downloadDir: File, nowMs: Long = System.currentTimeMillis()): OrphanPartCleanupResult {
    var scanned = 0
    var deleted = 0
    var kept = 0
    try {
        val staleAfterMs = TimeUnit.DAYS.toMillis(7)
        val parts = downloadDir.listFiles { _, name -> name.endsWith(".part") } ?: emptyArray()
        for (part in parts) {
            scanned += 1
            val ageMs = nowMs - part.lastModified()
            if (ageMs > staleAfterMs) {
                val size = part.length()
                if (part.delete()) {
                    deleted += 1
                    android.util.Log.i("DeviceAgentService", "delete orphan: name=${part.name}, age=${TimeUnit.MILLISECONDS.toDays(ageMs)}d, size=$size")
                } else {
                    kept += 1
                    android.util.Log.w("DeviceAgentService", "delete orphan failed: name=${part.name}, age=${TimeUnit.MILLISECONDS.toDays(ageMs)}d, size=$size")
                }
            } else {
                kept += 1
            }
        }
    } catch (e: Exception) {
        android.util.Log.w("DeviceAgentService", "orphan .part cleanup failed: ${e.message}")
    }
    android.util.Log.i("DeviceAgentService", "orphan .part cleanup: scanned=$scanned, deleted=$deleted, kept=$kept")
    return OrphanPartCleanupResult(scanned, deleted, kept)
}

private fun resumableDownload(
    url: String,
    dest: File,
    deviceId: String,
    expectedDigest: String,
    digestAlgo: String,
    enableHeadProbe: Boolean,
    progress: ((Long, Long) -> Unit)? = null
): Boolean {
    val partFile = File(dest.absolutePath + ".part")

    fun safeUrlLabel(raw: String): String {
        return try {
            val u = URL(raw)
            (u.host + u.path).take(64)
        } catch (_: Exception) {
            "<invalid-url>"
        }
    }

    fun parseContentRangeStart(contentRange: String?): Long? {
        if (contentRange.isNullOrBlank()) return null
        val match = Regex("""bytes\s+(\d+)-\d+/\d+|\*""").find(contentRange) ?: return null
        return match.groupValues.getOrNull(1)?.toLongOrNull()
    }

    fun parseContentRangeTotal(contentRange: String?): Long {
        if (contentRange.isNullOrBlank()) return -1L
        return contentRange.substringAfterLast('/', "").toLongOrNull() ?: -1L
    }

    fun notifyProgress(absBytes: Long, totalBytes: Long) {
        try {
            progress?.invoke(absBytes, totalBytes)
        } catch (e: Exception) {
            android.util.Log.w("DeviceAgentService", "progress callback threw: ${e.message}")
        }
    }

    fun seedDigestFromPart(digest: MessageDigest, existingSize: Long): Boolean {
        val startedAt = System.currentTimeMillis()
        var seeded = 0L
        return try {
            java.io.FileInputStream(partFile).use { input ->
                val buf = ByteArray(8192)
                var n: Int
                while (input.read(buf).also { n = it } != -1) {
                    digest.update(buf, 0, n)
                    seeded += n.toLong()
                }
            }
            val took = System.currentTimeMillis() - startedAt
            android.util.Log.i("DeviceAgentService", "seed digest from .part: bytes=$seeded, took=${took}ms")
            seeded == existingSize
        } catch (e: Exception) {
            android.util.Log.w("DeviceAgentService", "seed digest from .part failed: ${e.message}")
            false
        }
    }

    fun movePartToDest(): Boolean {
        try { if (dest.exists()) dest.delete() } catch (_: Exception) {}
        val renamed = partFile.renameTo(dest)
        android.util.Log.i("DeviceAgentService", "rename .part -> ${dest.name}: ok=$renamed")
        if (renamed) return true

        android.util.Log.e("DeviceAgentService", "rename .part -> ${dest.name}: ok=false, falling back to copy")
        return try {
            java.io.FileInputStream(partFile).use { input ->
                java.io.FileOutputStream(dest, false).use { output ->
                    val buf = ByteArray(8192)
                    var n: Int
                    while (input.read(buf).also { n = it } != -1) {
                        output.write(buf, 0, n)
                    }
                }
            }
            partFile.delete()
            true
        } catch (e: Exception) {
            android.util.Log.e("DeviceAgentService", "copy .part -> ${dest.name} failed: ${e.message}")
            try { dest.delete() } catch (_: Exception) {}
            false
        }
    }

    fun verifyCompletePart(existingSize: Long): Boolean {
        val digest = MessageDigest.getInstance(digestAlgo)
        if (!seedDigestFromPart(digest, existingSize)) {
            partFile.delete()
            return false
        }
        notifyProgress(existingSize, existingSize)
        val actual = digest.digest().joinToString("") { "%02x".format(it) }
        val ok = expectedDigest.isEmpty() || actual.equals(expectedDigest, true)
        android.util.Log.i("DeviceAgentService", "$digestAlgo ${if (ok) "ok" else "MISMATCH"}: expected=$expectedDigest actual=$actual")
        if (!ok) {
            partFile.delete()
            try { dest.delete() } catch (_: Exception) {}
            return false
        }
        return movePartToDest()
    }

    var existingSize = if (partFile.exists()) partFile.length() else 0L
    android.util.Log.i("DeviceAgentService", "resumableDownload: algo=$digestAlgo, headProbe=$enableHeadProbe, existingSize=$existingSize")
    while (true) {
        var conn: HttpURLConnection? = null
        try {
            if (enableHeadProbe && existingSize > 0) {
                var headConn: HttpURLConnection? = null
                val startedAt = System.currentTimeMillis()
                try {
                    headConn = URL(url).openConnection() as HttpURLConnection
                    headConn.requestMethod = "HEAD"
                    headConn.setRequestProperty("X-Device-ID", deviceId)
                    headConn.connectTimeout = 3000
                    headConn.readTimeout = 5000
                    val code = headConn.responseCode
                    val serverSize = headConn.getHeaderField("X-File-Size")?.toLongOrNull()
                    val serverSha = headConn.getHeaderField("X-File-SHA256")
                    val took = System.currentTimeMillis() - startedAt
                    android.util.Log.i("DeviceAgentService", "HEAD probe: code=$code, X-File-Size=$serverSize, X-File-SHA256=$serverSha, took=${took}ms")
                    if (code != HttpURLConnection.HTTP_OK) {
                        android.util.Log.w("DeviceAgentService", "HEAD failed (code=$code), falling back to GET with Range")
                    } else if (!serverSha.isNullOrBlank() && expectedDigest.isNotEmpty() && !serverSha.equals(expectedDigest, true)) {
                        android.util.Log.w("DeviceAgentService", "HEAD SHA mismatch: expected=$expectedDigest, server=$serverSha, dropping .part")
                        partFile.delete()
                        existingSize = 0L
                    } else if (serverSize != null && existingSize >= serverSize) {
                        android.util.Log.i("DeviceAgentService", "HEAD says download already complete, verifying .part directly")
                        return if (verifyCompletePart(existingSize)) {
                            true
                        } else {
                            existingSize = 0L
                            continue
                        }
                    }
                } catch (e: Exception) {
                    android.util.Log.w("DeviceAgentService", "HEAD failed (${e.message}), falling back to GET with Range")
                } finally {
                    headConn?.disconnect()
                }
            }
            val useRange = existingSize > 0
            android.util.Log.i("DeviceAgentService", "download ${safeUrlLabel(url)} existingSize=$existingSize, useRange=$useRange")
            conn = URL(url).openConnection() as HttpURLConnection
            conn.requestMethod = "GET"; conn.connectTimeout = 5000; conn.readTimeout = 30000
            conn.setRequestProperty("Accept", "application/octet-stream")
            conn.setRequestProperty("X-Device-ID", deviceId)
            if (useRange) conn.setRequestProperty("Range", "bytes=${existingSize}-")

            val code = conn.responseCode
            val contentRange = conn.getHeaderField("Content-Range")
            android.util.Log.i("DeviceAgentService", "HTTP code=$code, Content-Range=$contentRange")

            when (code) {
                HttpURLConnection.HTTP_NOT_FOUND -> {
                    partFile.delete()
                    return false
                }
                416 -> {
                    android.util.Log.w("DeviceAgentService", "416 detected, dropping .part and retrying full download")
                    partFile.delete()
                    existingSize = 0L
                    conn.disconnect()
                    continue
                }
                HttpURLConnection.HTTP_OK -> {
                    if (useRange) android.util.Log.w("DeviceAgentService", "200 OK ignoring Range, restarting from 0")
                    existingSize = 0L
                }
                HttpURLConnection.HTTP_PARTIAL -> {
                    val start = parseContentRangeStart(contentRange)
                    if (start != existingSize) {
                        android.util.Log.w("DeviceAgentService", "Content-Range mismatch: start=$start existingSize=$existingSize, restarting")
                        partFile.delete()
                        existingSize = 0L
                        conn.disconnect()
                        continue
                    }
                }
                else -> {
                    return false
                }
            }

            val totalBytes = when (code) {
                HttpURLConnection.HTTP_PARTIAL -> {
                    val total = parseContentRangeTotal(contentRange)
                    if (total > 0) total else {
                        val contentLength = conn.contentLengthLong
                        if (contentLength >= 0) existingSize + contentLength else -1L
                    }
                }
                HttpURLConnection.HTTP_OK -> {
                    val contentLength = conn.contentLengthLong
                    if (contentLength >= 0) contentLength else -1L
                }
                else -> -1L
            }
            val digest = MessageDigest.getInstance(digestAlgo)
            val append = code == HttpURLConnection.HTTP_PARTIAL && existingSize > 0
            if (append && !seedDigestFromPart(digest, existingSize)) {
                partFile.delete()
                existingSize = 0L
                conn.disconnect()
                continue
            }

            var downloadedBytes = existingSize
            java.io.FileOutputStream(partFile, append).use { fos ->
                conn.inputStream.use { inp ->
                    val buf = ByteArray(8192)
                    var n: Int
                    while (inp.read(buf).also { n = it } != -1) {
                        digest.update(buf, 0, n)
                        fos.write(buf, 0, n)
                        downloadedBytes += n.toLong()
                        notifyProgress(downloadedBytes, totalBytes)
                    }
                }
            }
            conn.disconnect()

            val actual = digest.digest().joinToString("") { "%02x".format(it) }
            val ok = expectedDigest.isEmpty() || actual.equals(expectedDigest, true)
            android.util.Log.i("DeviceAgentService", "$digestAlgo ${if (ok) "ok" else "MISMATCH"}: expected=$expectedDigest actual=$actual")
            if (!ok) {
                partFile.delete()
                try { dest.delete() } catch (_: Exception) {}
                return false
            }
            return movePartToDest()
        } catch (e: java.io.IOException) {
            android.util.Log.e("DeviceAgentService", "download error: ${e.message}")
            conn?.disconnect()
            return false
        } catch (e: Exception) {
            android.util.Log.e("DeviceAgentService", "download error: ${e.message}")
            conn?.disconnect()
            return false
        } finally {
            conn?.disconnect()
        }
    }
}

object UpgradingMark {
    private const val MARK_FILE = "upgrading"
    private const val TIMEOUT_MS = 5 * 60 * 1000L
    private fun markFile(svc: Service) = File(svc.filesDir, MARK_FILE)
    fun isUpgrading(svc: Service): Boolean {
        return try {
            val f = markFile(svc)
            if (!f.exists()) return false
            val elapsed = System.currentTimeMillis() - JSONObject(f.readText()).optLong("timestamp", 0)
            if (elapsed > TIMEOUT_MS) { clear(svc); false } else true
        } catch (e: Exception) { false }
    }
    fun write(svc: Service, cmdId: String) {
        try {
            val f = markFile(svc)
            val tmp = File(f.parentFile, f.name + ".tmp")
            tmp.writeText(JSONObject().apply { put("command_id", cmdId); put("timestamp", System.currentTimeMillis()) }.toString())
            tmp.renameTo(f)
        } catch (e: Exception) {}
    }
    fun clear(svc: Service) {
        try { markFile(svc).delete() } catch (e: Exception) {}
    }
}

class DeviceAgentService : Service() {
    companion object {
        private const val TAG = "DeviceAgentService"
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "device_agent_channel"

        init {
            try {
                System.loadLibrary("device-agent")
                android.util.Log.i(TAG, "native lib loaded")
            } catch (e: UnsatisfiedLinkError) {
                android.util.Log.e(TAG, "loadLibrary failed: ${e.message}")
            }
        }

        @JvmStatic
        external fun nativeStart(service: Any, host: String, port: Int, deviceId: String): Int
        @JvmStatic
        external fun nativeStop()
        @JvmStatic
        external fun nativeReportReleaseStatus(batchId: String, fileId: String, status: String, downloadedBytes: Long, errorCode: String, errorMessage: String): Boolean

        /** 根据 BuildConfig flavor 创建对应的安装器 */
        private fun createInstaller(service: DeviceAgentService): IAppInstaller = when (BuildConfig.INSTALL_MODE) {
            "silent"  -> SilentInstaller(service)
            "custom"  -> CustomInstaller(service)
            else      -> NormalInstaller(service)
        }
    }

    // 安装器（策略模式，由 productFlavor 决定具体实现）
    private val installer: IAppInstaller by lazy { createInstaller(this) }
    internal data class P2PDownloadContext(
        val batchId: String,
        val fileId: String,
        val commandId: String,
        val sha256: String,
        val localPath: String,
        var lastReportTimeMs: Long = 0L,
        var lastReportBytes: Long = 0L
    )
    private val p2pLock = Any()
    private var activeP2PContext: P2PDownloadContext? = null

    override fun onCreate() {
        super.onCreate()
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val savedUrl: String? = prefs.getString(KEY_SERVER_URL, null)
        cfgServerUrl = if (!savedUrl.isNullOrEmpty()) savedUrl else BuildConfig.DEFAULT_SERVER_URL
        val savedId: String? = prefs.getString(KEY_DEVICE_ID, null)
        cfgDeviceId = if (!savedId.isNullOrEmpty()) savedId else BuildConfig.DEFAULT_DEVICE_ID
        android.util.Log.i(TAG, "DeviceAgentService onCreate cfgServerUrl=$cfgServerUrl cfgDeviceId=$cfgDeviceId")
        mainServiceRef = WeakReference(this)

        // 订阅安装完成事件（观察者模式）
        InstallEventBus.subscribe { event -> handleInstallEvent(event) }

        try { File(filesDir, "upgrading").delete(); File(filesDir, "upgrading.tmp").delete(); File(filesDir, "skip_cmds.txt").delete(); File(filesDir, "pending_upgrade").delete() } catch (e: Exception) {}
        // 取消可能残留的重启闹钟（上次启动成功，不需要了）
        RestartReceiver.cancelRestart(this)
        mkChannel(); startForeground(NOTIFICATION_ID, mkNotification(IDLE_NOTIFICATION_TEXT))
        // 方案 B: gRPC streaming 替代 HTTP polling
        // gRPC 端口固定 9090，HTTP API 端口 8080
        val (parsedHost, _) = try {
            val u = java.net.URL(cfgServerUrl)
            Pair(u.host, if (u.port > 0) u.port else 8080)
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Failed to parse server URL: $cfgServerUrl", e)
            Pair("localhost", 8080)
        }
        val grpcPort = 9090
        android.util.Log.i(TAG, "Starting native gRPC: host=$parsedHost port=$grpcPort")
        val ret = nativeStart(this, parsedHost, grpcPort, cfgDeviceId)
        android.util.Log.i(TAG, "nativeStart returned: $ret")
        android.os.Handler(mainLooper).postDelayed({
            completeRecoveredInstallIfNeeded()
        }, 3_000)
        // 15秒后检查本地 download 目录，如有 APK 则自升级
        android.os.Handler(mainLooper).postDelayed({
            android.util.Log.i(TAG, "Checking for local APK to self-upgrade...")
            val downloadDir = File(filesDir, "downloads")
            val apks = downloadDir.listFiles { _, name -> name.endsWith(".apk") }
            if (apks != null && apks.isNotEmpty()) {
                val apkFile = apks.maxByOrNull { it.lastModified() }
                if (apkFile != null && apkFile.exists()) {
                    // 防不完整下载：文件最后修改时间超过 10 秒才尝试安装
                    val age = System.currentTimeMillis() - apkFile.lastModified()
                    if (age < 10_000) {
                        android.util.Log.i(TAG, "APK too fresh (${age}ms old), skipping self-upgrade to avoid partial file")
                    } else {
                        // 检查 APK 版本：与当前相同 → 上次安装残留，删除
                        val apkInfo = packageManager.getPackageArchiveInfo(apkFile.absolutePath, 0)
                        val apkVersion = apkInfo?.longVersionCode ?: 0
                        val curVersion = packageManager.getPackageInfo(packageName, 0).longVersionCode
                        if (apkVersion > 0 && apkVersion == curVersion) {
                            android.util.Log.i(TAG, "Stale APK (v$apkVersion matches current), deleting: ${apkFile.name}")
                            apkFile.delete()
                            return@postDelayed
                        }

                        android.util.Log.i(TAG, "Found local APK: ${apkFile.absolutePath} (${apkFile.length()} bytes) v$apkVersion, attempting self-install via ${installer.mode}")
                        UpgradingMark.write(this@DeviceAgentService, "self_upgrade")
                        RestartReceiver.scheduleRestart(this@DeviceAgentService)
                        // 记录当前版本号，RestartReceiver 用于判断升级是否完成
                        try { File(filesDir, "pending_upgrade").writeText(packageManager.getPackageInfo(packageName, 0).longVersionCode.toString()) } catch (_: Exception) {}
                        installer.install(apkFile) { result ->
                            if (result.success) {
                                android.util.Log.i(TAG, "Self-upgrade SUCCESS via ${installer.mode}")
                                UpgradingMark.clear(this@DeviceAgentService)
                                apkFile.delete()  // 升级成功，删除源 APK 避免重复通知
                            } else {
                                android.util.Log.e(TAG, "Self-upgrade FAILED: ${result.message}, will retry on next restart")
                                UpgradingMark.clear(this@DeviceAgentService)
                                removeSkip("self_upgrade")
                            }
                        }
                    }
                }
            } else {
                android.util.Log.i(TAG, "No local APK found in ${downloadDir.absolutePath}")
            }
        }, 15_000)
        cleanupOrphanPartFiles(File(filesDir, "downloads"))
    }
    override fun onStartCommand(i: Intent?, f: Int, s: Int): Int {
        val urlExtra = i?.getStringExtra("server_url")
        if (urlExtra != null) {
            cfgServerUrl = urlExtra
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(KEY_SERVER_URL, urlExtra).apply()
            android.util.Log.i(TAG, "cfgServerUrl overridden: $urlExtra")
        }
        val idExtra = i?.getStringExtra("device_id")
        if (idExtra != null) {
            cfgDeviceId = idExtra
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(KEY_DEVICE_ID, idExtra).apply()
            android.util.Log.i(TAG, "cfgDeviceId overridden: $idExtra")
        }
        return START_STICKY
    }
    override fun onBind(i: Intent?): IBinder? = null
    override fun onDestroy() {
        nativeStop()
        mainServiceRef = null
        super.onDestroy()
    }
    private fun mkChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "device-agent", NotificationManager.IMPORTANCE_HIGH))
    }
    // ─── JNI callbacks (called from C++ native code) ─────────────

    fun showToast(msg: String) {
        android.os.Handler(mainLooper).post {
            android.widget.Toast.makeText(this, msg, android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    fun onCommandResult(cmdId: String, status: String) {
        android.util.Log.i(TAG, "onCommandResult: cmdId=$cmdId status=$status")
    }

    // ─── onReboot：JNI fallback reboot（父进程调用）───
    // 当 fork 子进程的系统级 reboot 全部失败时，由 C++ 层回调
    // 在这里尝试 Android 原生 API 方式重启
    fun onReboot() {
        android.util.Log.i(TAG, "onReboot: JNI fallback reboot started")
        Thread {
            try {
                // 方式 1：PowerManager.reboot（需要 REBOOT 权限，系统签名 app）
                val pm = getSystemService(android.content.Context.POWER_SERVICE) as android.os.PowerManager
                pm.reboot("device-agent")
                android.util.Log.i(TAG, "PowerManager.reboot() succeeded")
            } catch (e: SecurityException) {
                android.util.Log.w(TAG, "PowerManager.reboot denied: ${e.message}, trying Runtime.exec")
                try {
                    // 方式 2：Runtime.exec svc power reboot（需要 shell 权限）
                    val proc = Runtime.getRuntime().exec(arrayOf("svc", "power", "reboot"))
                    val exitCode = proc.waitFor()
                    android.util.Log.i(TAG, "svc power reboot exit=$exitCode")
                } catch (e2: Exception) {
                    android.util.Log.w(TAG, "svc power reboot failed: ${e2.message}, trying su")
                    try {
                        // 方式 3：su -c reboot（需要 root）
                        val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", "reboot"))
                        val exitCode = proc.waitFor()
                        android.util.Log.i(TAG, "su -c reboot exit=$exitCode")
                    } catch (e3: Exception) {
                        android.util.Log.e(TAG, "All JNI reboot methods failed: ${e3.message}")
                    }
                }
            } catch (e: Exception) {
                android.util.Log.e(TAG, "onReboot unexpected error: ${e.message}")
            }
        }.start()
    }

    fun onDownloadReady(batchId: String, fileId: String, fileType: String, downloadUrl: String, sha256: String, fileSize: Long, commandId: String) {
        android.util.Log.i(TAG, "onDownloadReady: cmd=$commandId batch=$batchId file=$fileId type=$fileType size=$fileSize")
        synchronized(inFlightCmds) { if (inFlightCmds.contains(fileId)) { android.util.Log.i(TAG, "Skip already in-flight: $fileId"); return } }
        Thread { handleDownloadAndInstall(batchId, fileId, commandId, downloadUrl, sha256) }.start()
    }

    fun onP2PStarted(batchId: String, fileId: String, commandId: String, sha256: String, localPath: String) {
        android.util.Log.i(TAG, "onP2PStarted: cmd=$commandId batch=$batchId file=$fileId path=$localPath")
        synchronized(p2pLock) {
            activeP2PContext = P2PDownloadContext(batchId, fileId, commandId, sha256, localPath)
        }
        reportReleaseStatus(batchId, fileId, "downloading")
        updateNotification("⏬ P2P 下载中...")
    }

    fun onP2PProgress(percent: Int, downloadedBytes: Long, totalBytes: Long) {
        val ctx = synchronized(p2pLock) { activeP2PContext } ?: return
        val nowMs = SystemClock.elapsedRealtime()
        val shouldReport = shouldReportProgress(
            ctx.lastReportTimeMs,
            ctx.lastReportBytes,
            nowMs,
            downloadedBytes,
            totalBytes
        )
        if (!shouldReport) return

        reportReleaseStatus(ctx.batchId, ctx.fileId, "downloading", downloadedBytes)
        updateNotification(buildDownloadText(downloadedBytes, totalBytes))
        android.util.Log.i(TAG, "p2p progress reported: pct=$percent abs=$downloadedBytes total=$totalBytes")
        synchronized(p2pLock) {
            activeP2PContext?.let {
                if (it.fileId == ctx.fileId) {
                    it.lastReportTimeMs = nowMs
                    it.lastReportBytes = downloadedBytes
                }
            }
        }
    }

    fun onP2PComplete(success: Boolean, errorMsg: String) {
        val ctx = synchronized(p2pLock) {
            val current = activeP2PContext
            activeP2PContext = null
            current
        }
        if (ctx == null) {
            android.util.Log.w(TAG, "onP2PComplete without active context: success=$success err=$errorMsg")
            return
        }

        if (!success) {
            android.util.Log.e(TAG, "P2P download failed: file=${ctx.fileId} err=$errorMsg")
            reportCommandStatus(ctx.commandId, "failed", "P2P download failed: $errorMsg")
            val errorCode = if (errorMsg.contains("sha256", ignoreCase = true)) "CHECKSUM_FAILED" else "NETWORK_ERROR"
            reportReleaseStatus(ctx.batchId, ctx.fileId, "download_failed", errorCode = errorCode, errorMessage = errorMsg)
            updateNotificationTemporarily("❌ P2P 下载失败")
            return
        }

        Thread {
            handleDownloadAndInstall(
                ctx.batchId,
                ctx.fileId,
                ctx.commandId,
                ctx.localPath,
                ctx.sha256,
                File(ctx.localPath)
            )
        }.start()
    }

    fun onUpgradeApp(apkUrl: String, md5: String, commandId: String) {
        android.util.Log.i(TAG, "onUpgradeApp: cmd=$commandId md5=$md5")
        Thread { handleUpgradeInstall(apkUrl, md5, commandId) }.start()
    }

    // ─── Download + Install helpers ─────────────────────────────

    private fun handleDownloadAndInstall(
        batchId: String,
        fileId: String,
        commandId: String,
        url: String,
        sha: String,
        predownloadedApk: File? = null
    ) {
        val svc = this
        if (UpgradingMark.isUpgrading(svc)) {
            android.util.Log.i(TAG, "Already upgrading, skip download_ready")
            return
        }
        synchronized(inFlightFiles) { if (inFlightFiles.contains(fileId)) return; inFlightFiles.add(fileId) }
        val dest = File(File(filesDir, "downloads").also { it.mkdirs() }, "$fileId.apk")
        reportReleaseStatus(batchId, fileId, "downloading")
        updateNotification("⏬ 正在下载...")
        var lastReportTimeMs = 0L
        var lastReportBytes = 0L
        val downloaded = if (predownloadedApk != null) {
            usePredownloadedApk(predownloadedApk, dest)
        } else {
            downloadFile(url, dest, sha) { absBytes, totalBytes ->
                val nowMs = SystemClock.elapsedRealtime()
                if (shouldReportProgress(lastReportTimeMs, lastReportBytes, nowMs, absBytes, totalBytes)) {
                    reportReleaseStatus(batchId, fileId, "downloading", absBytes)
                    updateNotification(buildDownloadText(absBytes, totalBytes))
                    val intervalMs = if (lastReportTimeMs == 0L) 0L else nowMs - lastReportTimeMs
                    val pct = if (totalBytes > 0) ((absBytes * 100) / totalBytes).coerceIn(0, 100) else -1
                    android.util.Log.i(TAG, "progress reported: abs=$absBytes total=$totalBytes pct=${pct}% intervalMs=$intervalMs")
                    lastReportTimeMs = nowMs
                    lastReportBytes = absBytes
                }
            }
        }
        if (!downloaded) {
            synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
            reportCommandStatus(commandId, "failed", "Download failed")
            reportReleaseStatus(batchId, fileId, "download_failed", errorCode = "NETWORK_ERROR", errorMessage = "Download failed")
            return
        }
        synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
        reportReleaseStatus(batchId, fileId, "downloaded", dest.length())
        android.util.Log.i(TAG, "Download OK, cmd=$commandId batch=$batchId file=$fileId, installing via ${installer.mode}")
        UpgradingMark.write(svc, fileId)
        reportReleaseStatus(batchId, fileId, "installing")
        updateNotification("📦 正在安装...")

        RestartReceiver.scheduleRestart(this)
        // 记录当前版本号，RestartReceiver 用于判断升级是否完成
        try { File(filesDir, "pending_upgrade").writeText(packageManager.getPackageInfo(packageName, 0).longVersionCode.toString()) } catch (_: Exception) {}
        writePendingInstall(batchId, fileId, commandId)
        installer.install(dest) { result ->
            if (result.success) {
                // 记录 sessionId → (batchId, fileId, commandId)，等 InstallEventBus 推送最终结果
                if (result.sessionId > 0) {
                    synchronized(pendingInstalls) {
                        pendingInstalls[result.sessionId] = PendingInstall(batchId, fileId, commandId)
                    }
                } else {
                    scheduleActionViewInstallTimeout(batchId, fileId, commandId, dest)
                }
                // Silent mode: install 是同步的，可能已经完成
                if (installer.mode == "silent") {
                    android.util.Log.i(TAG, "Silent install: ${result.message}")
                    // InstallResultReceiver 会通过 bus 回调最终结果
                } else {
                    android.util.Log.i(TAG, "Install submitted: ${result.message}")
                }
            } else {
                android.util.Log.e(TAG, "${installer.mode} install failed: ${result.message}")
                reportCommandStatus(commandId, "failed", result.message)
                reportReleaseStatus(batchId, fileId, "install_failed", errorCode = "INSTALL_ERROR", errorMessage = result.message)
                clearPendingInstall()
                UpgradingMark.clear(svc)
                removeSkip(fileId)
                try { dest.delete() } catch (_: Exception) {}
            }
        }
    }

    private fun usePredownloadedApk(source: File, dest: File): Boolean {
        if (!source.exists() || !source.isFile) {
            android.util.Log.e(TAG, "predownloaded APK missing: ${source.absolutePath}")
            return false
        }
        if (source.absolutePath == dest.absolutePath) {
            return true
        }
        return try {
            dest.parentFile?.mkdirs()
            try { if (dest.exists()) dest.delete() } catch (_: Exception) {}
            if (source.renameTo(dest)) {
                true
            } else {
                source.inputStream().use { input ->
                    dest.outputStream().use { output ->
                        input.copyTo(output)
                    }
                }
                try { source.delete() } catch (_: Exception) {}
                true
            }
        } catch (e: Exception) {
            android.util.Log.e(TAG, "use predownloaded APK failed: ${e.message}")
            try { dest.delete() } catch (_: Exception) {}
            false
        }
    }

    private fun handleUpgradeInstall(url: String, md5: String, commandId: String) {
        val svc = this
        if (UpgradingMark.isUpgrading(svc)) return
        if (url.isEmpty()) return
        val fid = stableUpgradeFileId(url, md5)
        val dest = File(File(filesDir, "downloads").also { it.mkdirs() }, "$fid.apk")
        if (!downloadFileMD5(url, dest, md5)) {
            reportCommandStatus(commandId, "failed", "Download failed"); return
        }
        UpgradingMark.write(svc, fid)
        RestartReceiver.scheduleRestart(this)
        // 记录当前版本号，RestartReceiver 用于判断升级是否完成
        try { File(filesDir, "pending_upgrade").writeText(packageManager.getPackageInfo(packageName, 0).longVersionCode.toString()) } catch (_: Exception) {}
        writePendingInstall("", fid, commandId)
        installer.install(dest) { result ->
            if (result.success) {
                if (result.sessionId > 0) {
                    synchronized(pendingInstalls) {
                        pendingInstalls[result.sessionId] = PendingInstall("", fid, commandId)
                    }
                } else {
                    scheduleActionViewInstallTimeout("", fid, commandId, dest)
                }
            } else {
                reportCommandStatus(commandId, "failed", result.message)
                clearPendingInstall()
                UpgradingMark.clear(svc); removeSkip(fid)
                try { dest.delete() } catch (_: Exception) {}
            }
        }
    }

    private fun scheduleActionViewInstallTimeout(batchId: String, fileId: String, commandId: String, apkFile: File) {
        android.util.Log.i(TAG, "schedule action-view install timeout: cmd=$commandId file=$fileId timeoutMs=$ACTION_VIEW_INSTALL_TIMEOUT_MS")
        android.os.Handler(mainLooper).postDelayed({
            failPendingActionViewInstallIfUnchanged(batchId, fileId, commandId, apkFile)
        }, ACTION_VIEW_INSTALL_TIMEOUT_MS)
    }

    private fun stableUpgradeFileId(url: String, md5: String): String {
        val key = "$url\n$md5"
        val digest = MessageDigest.getInstance("SHA-256")
            .digest(key.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it) }
        return "upgrade_${digest.take(16)}"
    }

    // ─── Reporting helpers ──────────────────────────────────────

    private fun reportCommandStatus(cmdId: String, status: String, msg: String) {
        try {
            val j = JSONObject().apply { put("command_id", cmdId); put("status", status); put("result_message", msg) }
            val c = URL("$cfgServerUrl/api/v1/devices/$cfgDeviceId/report").openConnection() as HttpURLConnection
            c.requestMethod = "POST"; c.connectTimeout = 5000; c.readTimeout = 10000
            c.setRequestProperty("Content-Type", "application/json"); c.doOutput = true
            c.outputStream.use { it.write(j.toString().toByteArray()) }
            android.util.Log.i(TAG, "Report: $cmdId status=$status httpCode=${c.responseCode}")
            c.disconnect()
        } catch (e: Exception) { android.util.Log.e(TAG, "Report failed: ${e.message}") }
    }

    private fun reportReleaseStatus(
        batchId: String,
        fileId: String,
        status: String,
        bytes: Long = 0,
        errorCode: String = "",
        errorMessage: String = ""
    ) {
        if (batchId.isEmpty()) return
        Thread {
            try { nativeReportReleaseStatus(batchId, fileId, status, bytes, errorCode, errorMessage) }
            catch (e: Exception) {
                android.util.Log.e(TAG, "reportReleaseStatus native: ${e.message}")
            }
        }.start()
    }

    private fun downloadFile(
        url: String,
        dest: File,
        sha: String,
        progress: ((absBytes: Long, totalBytes: Long) -> Unit)? = null
    ): Boolean = resumableDownload(url, dest, cfgDeviceId, sha, "SHA-256", enableHeadProbe = true, progress)

    private fun downloadFileMD5(url: String, dest: File, md5: String): Boolean {
        return resumableDownload(url, dest, cfgDeviceId, md5, "MD5", enableHeadProbe = false, progress = null)
    }

    private fun mkNotification(text: String): Notification {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            Notification.Builder(this, CHANNEL_ID).setContentTitle("device-agent").setContentText(text)
                .setSmallIcon(R.drawable.ic_notification).setPriority(Notification.PRIORITY_HIGH).build()
        else
            @Suppress("DEPRECATION")
            Notification.Builder(this).setContentTitle("device-agent").setContentText(text)
                .setSmallIcon(R.drawable.ic_notification).setPriority(Notification.PRIORITY_HIGH).build()
    }

    /** 更新前景通知文字 */
    private fun updateNotification(text: String) {
        try {
            val nm = getSystemService(NotificationManager::class.java)
            val notification = mkNotification(text)
            nm.notify(NOTIFICATION_ID, notification)
        } catch (e: Exception) {
            android.util.Log.e(TAG, "updateNotification failed: ${e.message}")
        }
    }

    private fun updateNotificationTemporarily(text: String) {
        updateNotification(text)
        android.os.Handler(mainLooper).postDelayed({
            if (!UpgradingMark.isUpgrading(this)) {
                updateNotification(IDLE_NOTIFICATION_TEXT)
            }
        }, TRANSIENT_NOTIFICATION_MS)
    }

    private val skipFile: File get() = File("/data/data/com.deviceagent/files/skip_cmds.txt")
    private val inFlightCmds = mutableSetOf<String>()
    private val inFlightFiles = mutableSetOf<String>()

    /** 等待安装结果的任务映射：sessionId → {batchId, fileId, commandId} */
    internal data class PendingInstall(val batchId: String, val fileId: String, val commandId: String)
    private val pendingInstalls = mutableMapOf<Int, PendingInstall>()

    private fun pendingInstallFile(): File = File(filesDir, PENDING_INSTALL_FILE)

    private fun writePendingInstall(batchId: String, fileId: String, commandId: String) {
        try {
            val curVersion = packageManager.getPackageInfo(packageName, 0).longVersionCode
            val marker = JSONObject().apply {
                put("batch_id", batchId)
                put("file_id", fileId)
                put("command_id", commandId)
                put("started_at", System.currentTimeMillis())
                put("version_code", curVersion)
            }
            pendingInstallFile().writeText(marker.toString())
            android.util.Log.i(TAG, "pending install marker written: cmd=$commandId batch=$batchId file=$fileId version=$curVersion")
        } catch (e: Exception) {
            android.util.Log.w(TAG, "write pending install marker failed: ${e.message}")
        }
    }

    private fun clearPendingInstall() {
        try { pendingInstallFile().delete() } catch (_: Exception) {}
    }

    private fun failPendingActionViewInstallIfUnchanged(batchId: String, fileId: String, commandId: String, apkFile: File) {
        val markerFile = pendingInstallFile()
        if (!markerFile.exists()) return
        try {
            val marker = JSONObject(markerFile.readText())
            val markerFileId = marker.optString("file_id", "")
            if (markerFileId != fileId) return

            val startedAt = marker.optLong("started_at", 0L)
            val oldVersion = marker.optLong("version_code", 0L)
            val info = packageManager.getPackageInfo(packageName, 0)
            val installChanged = info.lastUpdateTime >= startedAt || info.longVersionCode != oldVersion
            if (installChanged) {
                android.util.Log.i(TAG, "action-view install timeout saw completed install: cmd=$commandId file=$fileId lastUpdate=${info.lastUpdateTime}")
                completeRecoveredInstallIfNeeded()
                return
            }

            android.util.Log.w(TAG, "action-view install timed out/cancelled: cmd=$commandId batch=$batchId file=$fileId")
            updateNotificationTemporarily("❌ 安装未完成")
            Thread { reportCommandStatus(commandId, "failed", "Install cancelled or timed out") }.start()
            if (batchId.isNotEmpty()) {
                reportReleaseStatus(
                    batchId,
                    fileId,
                    "cancelled",
                    errorCode = "BUSINESS_ERROR",
                    errorMessage = "Install cancelled or timed out"
                )
            }
            clearPendingInstall()
            RestartReceiver.cancelRestart(this)
            try { File(filesDir, "pending_upgrade").delete() } catch (_: Exception) {}
            UpgradingMark.clear(this)
            removeSkip(fileId)
            try { apkFile.delete() } catch (_: Exception) {}
        } catch (e: Exception) {
            android.util.Log.e(TAG, "action-view install timeout check failed: ${e.message}")
        }
    }

    private fun completeRecoveredInstallIfNeeded() {
        val markerFile = pendingInstallFile()
        if (!markerFile.exists()) return
        try {
            val marker = JSONObject(markerFile.readText())
            val batchId = marker.optString("batch_id", "")
            val fileId = marker.optString("file_id", "")
            val commandId = marker.optString("command_id", "")
            val startedAt = marker.optLong("started_at", 0L)
            val oldVersion = marker.optLong("version_code", 0L)
            val info = packageManager.getPackageInfo(packageName, 0)
            val installChanged = info.lastUpdateTime >= startedAt || info.longVersionCode != oldVersion
            if (!installChanged || fileId.isEmpty()) {
                android.util.Log.i(TAG, "pending install not completed yet: file=$fileId started=$startedAt lastUpdate=${info.lastUpdateTime}")
                return
            }
            android.util.Log.i(TAG, "Recovered install completion: cmd=$commandId batch=$batchId file=$fileId version=${info.longVersionCode} lastUpdate=${info.lastUpdateTime}")
            // 旧格式 JSON 可能无 command_id，此时仅上报 release 状态（不依赖 command_id）
            if (commandId.isNotEmpty()) {
                reportCommandStatus(commandId, "completed", "Installed after package restart")
            }
            if (batchId.isNotEmpty()) reportReleaseStatus(batchId, fileId, "installed")
            clearPendingInstall()
            RestartReceiver.cancelRestart(this)
            try { File(filesDir, "pending_upgrade").delete() } catch (_: Exception) {}
            UpgradingMark.clear(this)
            removeSkip(fileId)
            try { File(filesDir, "downloads/$fileId.apk").delete() } catch (_: Exception) {}
        } catch (e: Exception) {
            android.util.Log.e(TAG, "complete recovered install failed: ${e.message}")
        }
    }

    /** 处理 InstallEventBus 推送的安装结果 */
    private fun handleInstallEvent(event: InstallEvent) {
        val info = synchronized(pendingInstalls) { pendingInstalls.remove(event.sessionId) } ?: return
        when (event) {
            is InstallEvent.Succeeded -> {
                val pkg = event.packageName ?: "unknown"
                android.util.Log.i(TAG, "Install SUCCESS: session=${event.sessionId} cmd=${info.commandId} batch=${info.batchId} file=${info.fileId}")
                updateNotification("✅ 安装成功: $pkg")
                RestartReceiver.cancelRestart(this)  // 进程没被杀→取消闹钟；进程被杀→这行不执行
                File(filesDir, "pending_upgrade").delete()
                reportCommandStatus(info.commandId, "completed", "Installed via ${installer.mode}")
                if (info.batchId.isNotEmpty()) reportReleaseStatus(info.batchId, info.fileId, "installed")
                clearPendingInstall()
                UpgradingMark.clear(this)
                removeSkip(info.fileId)
                try { File(filesDir, "downloads/${info.fileId}.apk").delete() } catch (_: Exception) {}
            }
            is InstallEvent.PendingUser -> {
                // Normal 模式等用户确认
                android.util.Log.i(TAG, "Install pending user confirm: session=${event.sessionId} cmd=${info.commandId} file=${info.fileId}")
                updateNotification("⏳ 等待确认安装...")
            }
            is InstallEvent.Failed -> {
                android.util.Log.e(TAG, "Install FAILED: session=${event.sessionId} cmd=${info.commandId} batch=${info.batchId} file=${info.fileId} err=${event.error}")
                updateNotification("❌ 安装失败: ${event.error}")
                RestartReceiver.cancelRestart(this)  // 失败了不需要重启
                File(filesDir, "pending_upgrade").delete()
                reportCommandStatus(info.commandId, "failed", event.error)
                if (info.batchId.isNotEmpty()) {
                    reportReleaseStatus(
                        info.batchId,
                        info.fileId,
                        "install_failed",
                        errorCode = "INSTALL_ERROR",
                        errorMessage = event.error
                    )
                }
                clearPendingInstall()
                UpgradingMark.clear(this)
                removeSkip(info.fileId)
            }
        }
    }

    // ── skip/retry helpers ──

    @Synchronized
    private fun markSkipped(cmdId: String): Boolean {
        if (inFlightCmds.contains(cmdId)) return true
        try {
            if (skipFile.exists() && skipFile.readText().split(",").filter { it.isNotEmpty() }.contains(cmdId)) {
                inFlightCmds.add(cmdId); return true
            }
        } catch (e: Exception) {}
        inFlightCmds.add(cmdId)
        try {
            val parts = (if (skipFile.exists()) skipFile.readText() else "").split(",").filter { it.isNotEmpty() }.toMutableList()
            parts.add(cmdId); skipFile.writeText(parts.takeLast(20).joinToString(","))
        } catch (e: Exception) {}
        return false
    }

    @Synchronized
    private fun removeSkip(cmdId: String) {
        inFlightCmds.remove(cmdId)
        try {
            val parts = (if (skipFile.exists()) skipFile.readText() else "").split(",").filter { it.isNotEmpty() }.toMutableList()
            parts.remove(cmdId); skipFile.writeText(parts.joinToString(","))
        } catch (e: Exception) {}
    }
}
