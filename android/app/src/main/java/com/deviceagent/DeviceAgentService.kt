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
import org.json.JSONObject



private const val PREFS_NAME = "device_agent_config"
private const val KEY_SERVER_URL = "server_url"
private const val KEY_DEVICE_ID = "device_id"
private var cfgServerUrl: String = ""
private var cfgDeviceId: String = ""
private var mainServiceRef: WeakReference<DeviceAgentService>? = null

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
        external fun nativeStart(service: Any, host: String, port: Int): Int
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
        mkChannel(); startForeground(NOTIFICATION_ID, mkNotification("device-agent running"))
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
        val ret = nativeStart(this, parsedHost, grpcPort)
        android.util.Log.i(TAG, "nativeStart returned: $ret")
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

    fun onDownloadReady(batchId: String, fileId: String, fileType: String, downloadUrl: String, sha256: String, fileSize: Long) {
        android.util.Log.i(TAG, "onDownloadReady: batch=$batchId file=$fileId type=$fileType url=$downloadUrl size=$fileSize")
        synchronized(inFlightCmds) { if (inFlightCmds.contains(fileId)) { android.util.Log.i(TAG, "Skip already in-flight: $fileId"); return } }
        Thread { handleDownloadAndInstall(batchId, fileId, downloadUrl, sha256) }.start()
    }

    fun onUpgradeApp(apkUrl: String, md5: String) {
        android.util.Log.i(TAG, "onUpgradeApp: url=$apkUrl md5=$md5")
        Thread { handleUpgradeInstall(apkUrl, md5) }.start()
    }

    // ─── Download + Install helpers ─────────────────────────────

    private fun handleDownloadAndInstall(batchId: String, fileId: String, url: String, sha: String) {
        val svc = this
        if (UpgradingMark.isUpgrading(svc)) {
            android.util.Log.i(TAG, "Already upgrading, skip download_ready")
            return
        }
        synchronized(inFlightFiles) { if (inFlightFiles.contains(fileId)) return; inFlightFiles.add(fileId) }
        val dest = File(File(filesDir, "downloads").also { it.mkdirs() }, "$fileId.apk")
        reportReleaseStatus(batchId, fileId, "downloading")
        updateNotification("⏬ 正在下载...")
        if (!downloadFile(url, dest, sha)) {
            synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
            reportCommandStatus(fileId, "failed", "Download failed")
            reportReleaseStatus(batchId, fileId, "download_failed")
            return
        }
        synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
        reportReleaseStatus(batchId, fileId, "downloaded", dest.length())
        android.util.Log.i(TAG, "Download OK, installing via ${installer.mode}")
        UpgradingMark.write(svc, fileId)
        reportCommandStatus(fileId, "installing", "Running ${installer.mode} installer")
        reportReleaseStatus(batchId, fileId, "installing")
        updateNotification("📦 正在安装...")

        RestartReceiver.scheduleRestart(this)
        // 记录当前版本号，RestartReceiver 用于判断升级是否完成
        try { File(filesDir, "pending_upgrade").writeText(packageManager.getPackageInfo(packageName, 0).longVersionCode.toString()) } catch (_: Exception) {}
        installer.install(dest) { result ->
            if (result.success) {
                // 记录 sessionId → (batchId, fileId)，等 InstallEventBus 推送最终结果
                if (result.sessionId > 0) {
                    synchronized(pendingInstalls) {
                        pendingInstalls[result.sessionId] = PendingInstall(batchId, fileId)
                    }
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
                reportCommandStatus(fileId, "failed", result.message)
                reportReleaseStatus(batchId, fileId, "install_failed")
                UpgradingMark.clear(svc)
                removeSkip(fileId)
                try { dest.delete() } catch (_: Exception) {}
            }
        }
    }

    private fun handleUpgradeInstall(url: String, md5: String) {
        val svc = this
        if (UpgradingMark.isUpgrading(svc)) return
        if (url.isEmpty()) return
        val fid = "upgrade_${System.currentTimeMillis()}"
        val dest = File(File(filesDir, "downloads").also { it.mkdirs() }, "$fid.apk")
        if (!downloadFileMD5(url, dest, md5)) {
            reportCommandStatus(fid, "failed", "Download failed"); return
        }
        UpgradingMark.write(svc, fid)
        reportCommandStatus(fid, "installing", "Running ${installer.mode} installer")
        RestartReceiver.scheduleRestart(this)
        // 记录当前版本号，RestartReceiver 用于判断升级是否完成
        try { File(filesDir, "pending_upgrade").writeText(packageManager.getPackageInfo(packageName, 0).longVersionCode.toString()) } catch (_: Exception) {}
        installer.install(dest) { result ->
            if (result.success) {
                if (result.sessionId > 0) {
                    synchronized(pendingInstalls) {
                        pendingInstalls[result.sessionId] = PendingInstall("", fid)
                    }
                }
            } else {
                reportCommandStatus(fid, "failed", result.message)
                UpgradingMark.clear(svc); removeSkip(fid)
            }
            try { dest.delete() } catch (_: Exception) {}
        }
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

    private fun reportReleaseStatus(batchId: String, fileId: String, status: String, bytes: Long = 0) {
        if (batchId.isEmpty()) return
        Thread {
            try { nativeReportReleaseStatus(batchId, fileId, status, bytes, "", "") }
            catch (e: Exception) {
                android.util.Log.e(TAG, "reportReleaseStatus native: ${e.message}")
            }
        }.start()
    }

    private fun downloadFile(url: String, dest: File, sha: String): Boolean {
        var conn: HttpURLConnection? = null
        return try {
            conn = URL(url).openConnection() as HttpURLConnection
            conn.requestMethod = "GET"; conn.connectTimeout = 5000; conn.readTimeout = 30000
            conn.setRequestProperty("Accept", "application/octet-stream")
            if (conn.responseCode != 200 && conn.responseCode != 206) { conn.disconnect(); return false }
            val d = java.security.MessageDigest.getInstance("SHA-256")
            java.io.FileOutputStream(dest).use { fos ->
                conn.inputStream.use { inp ->
                    val buf = ByteArray(8192); var n: Int
                    while (inp.read(buf).also { n = it } != -1) { d.update(buf, 0, n); fos.write(buf, 0, n) }
                }
            }; conn.disconnect()
            val actual = d.digest().joinToString("") { "%02x".format(it) }
            if (sha.isNotEmpty() && !actual.equals(sha, true)) { android.util.Log.e(TAG, "SHA256 mismatch"); dest.delete(); return false }
            android.util.Log.i(TAG, "SHA256 OK: $actual"); true
        } catch (e: Exception) { android.util.Log.e(TAG, "download error: ${e.message}"); conn?.disconnect(); false }
    }

    private fun downloadFileMD5(url: String, dest: File, md5: String): Boolean {
        var conn: HttpURLConnection? = null
        return try {
            conn = URL(url).openConnection() as HttpURLConnection
            conn.requestMethod = "GET"; conn.connectTimeout = 5000; conn.readTimeout = 30000
            conn.setRequestProperty("Accept", "application/octet-stream")
            if (conn.responseCode != 200 && conn.responseCode != 206) { conn.disconnect(); return false }
            val d = java.security.MessageDigest.getInstance("MD5")
            java.io.FileOutputStream(dest).use { fos ->
                conn.inputStream.use { inp ->
                    val buf = ByteArray(8192); var n: Int
                    while (inp.read(buf).also { n = it } != -1) { d.update(buf, 0, n); fos.write(buf, 0, n) }
                }
            }; conn.disconnect()
            val actual = d.digest().joinToString("") { "%02x".format(it) }
            if (md5.isNotEmpty() && !actual.equals(md5, true)) { android.util.Log.e(TAG, "MD5 mismatch"); dest.delete(); return false }
            android.util.Log.i(TAG, "MD5 OK: $actual"); true
        } catch (e: Exception) { android.util.Log.e(TAG, "download error: ${e.message}"); conn?.disconnect(); false }
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

    private val skipFile: File get() = File("/data/data/com.deviceagent/files/skip_cmds.txt")
    private val inFlightCmds = mutableSetOf<String>()
    private val inFlightFiles = mutableSetOf<String>()

    /** 等待安装结果的任务映射：sessionId → {batchId, fileId} */
    private data class PendingInstall(val batchId: String, val fileId: String)
    private val pendingInstalls = mutableMapOf<Int, PendingInstall>()

    /** 处理 InstallEventBus 推送的安装结果 */
    private fun handleInstallEvent(event: InstallEvent) {
        val info = synchronized(pendingInstalls) { pendingInstalls.remove(event.sessionId) } ?: return
        when (event) {
            is InstallEvent.Succeeded -> {
                val pkg = event.packageName ?: "unknown"
                android.util.Log.i(TAG, "Install SUCCESS: session=${event.sessionId} batch=${info.batchId} file=${info.fileId}")
                updateNotification("✅ 安装成功: $pkg")
                RestartReceiver.cancelRestart(this)  // 进程没被杀→取消闹钟；进程被杀→这行不执行
                File(filesDir, "pending_upgrade").delete()
                reportCommandStatus(info.fileId, "completed", "Installed via ${installer.mode}")
                if (info.batchId.isNotEmpty()) reportReleaseStatus(info.batchId, info.fileId, "installed")
                UpgradingMark.clear(this)
                removeSkip(info.fileId)
                try { File(filesDir, "downloads/${info.fileId}.apk").delete() } catch (_: Exception) {}
            }
            is InstallEvent.PendingUser -> {
                // Normal 模式等用户确认
                android.util.Log.i(TAG, "Install pending user confirm: session=${event.sessionId} file=${info.fileId}")
                updateNotification("⏳ 等待确认安装...")
            }
            is InstallEvent.Failed -> {
                android.util.Log.e(TAG, "Install FAILED: session=${event.sessionId} batch=${info.batchId} file=${info.fileId} err=${event.error}")
                updateNotification("❌ 安装失败: ${event.error}")
                RestartReceiver.cancelRestart(this)  // 失败了不需要重启
                File(filesDir, "pending_upgrade").delete()
                reportCommandStatus(info.fileId, "failed", event.error)
                if (info.batchId.isNotEmpty()) reportReleaseStatus(info.batchId, info.fileId, "install_failed")
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
