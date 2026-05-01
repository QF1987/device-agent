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
    }
    override fun onCreate() {
        super.onCreate()
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val savedUrl: String? = prefs.getString(KEY_SERVER_URL, null)
        cfgServerUrl = if (!savedUrl.isNullOrEmpty()) savedUrl else BuildConfig.DEFAULT_SERVER_URL
        val savedId: String? = prefs.getString(KEY_DEVICE_ID, null)
        cfgDeviceId = if (!savedId.isNullOrEmpty()) savedId else BuildConfig.DEFAULT_DEVICE_ID
        android.util.Log.i(TAG, "DeviceAgentService onCreate cfgServerUrl=$cfgServerUrl cfgDeviceId=$cfgDeviceId")
        mainServiceRef = WeakReference(this)
        try { File(filesDir, "upgrading").delete(); File(filesDir, "upgrading.tmp").delete(); File(filesDir, "skip_cmds.txt").delete() } catch (e: Exception) {}
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
                    android.util.Log.i(TAG, "Found local APK: ${apkFile.absolutePath}, attempting self-install")
                    UpgradingMark.write(this@DeviceAgentService, "self_upgrade")
                    val result = runPmInstall(apkFile.absolutePath)
                    android.util.Log.i(TAG, "Self-install result: $result")
                    if (result.startsWith("SUCCESS")) {
                        android.util.Log.i(TAG, "Self-upgrade SUCCESS, stopping")
                        UpgradingMark.clear(this@DeviceAgentService)
                    } else {
                        android.util.Log.e(TAG, "Self-upgrade FAILED: $result, will retry on next restart")
                        UpgradingMark.clear(this@DeviceAgentService)
                        removeSkip("self_upgrade")
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
        if (!downloadFile(url, dest, sha)) {
            synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
            reportCommandStatus(fileId, "failed", "Download failed")
            reportReleaseStatus(batchId, fileId, "download_failed")
            return
        }
        synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
        reportReleaseStatus(batchId, fileId, "downloaded", dest.length())
        android.util.Log.i(TAG, "Download OK, running pm install")
        UpgradingMark.write(svc, fileId)
        reportCommandStatus(fileId, "installing", "Copying APK")
        reportReleaseStatus(batchId, fileId, "installing")
        val tmp = File("/data/local/tmp/$fileId.apk")
        try { dest.inputStream().use { i -> tmp.outputStream().use { o -> i.copyTo(o) } } } catch (e: Exception) {
            android.util.Log.e(TAG, "Copy failed: ${e.message}")
            reportCommandStatus(fileId, "failed", "Copy failed")
            reportReleaseStatus(batchId, fileId, "install_failed")
            UpgradingMark.clear(svc); removeSkip(fileId); return
        }
        reportCommandStatus(fileId, "installing", "Running pm install")
        val result = runPmInstall(tmp.absolutePath)
        android.util.Log.i("DeviceAgentService", "pm install result: $result")
        if (result.startsWith("SUCCESS")) {
            val vc = try { packageManager.getPackageInfo(packageName, 0).longVersionCode } catch (e: Exception) { 0L }
            reportCommandStatus(fileId, "completed", "Installed version $vc")
            reportReleaseStatus(batchId, fileId, "installed")
            UpgradingMark.clear(svc)
            try { tmp.delete() } catch (e: Exception) {}
            try { dest.delete() } catch (e: Exception) {}
        } else {
            android.util.Log.e("DeviceAgentService", "pm install failed: $result")
            reportCommandStatus(fileId, "failed", result)
            reportReleaseStatus(batchId, fileId, "install_failed")
            UpgradingMark.clear(svc); removeSkip(fileId)
            try { tmp.delete() } catch (e: Exception) {}
        }
    }

    private fun handleUpgradeInstall(url: String, md5: String) {
        val svc = this
        if (UpgradingMark.isUpgrading(svc)) return
        if (url.isNotEmpty()) {
            val fid = "upgrade_${System.currentTimeMillis()}"
            val dest = File(File(filesDir, "downloads").also { it.mkdirs() }, "$fid.apk")
            if (!downloadFileMD5(url, dest, md5)) {
                reportCommandStatus(fid, "failed", "Download failed"); return
            }
            UpgradingMark.write(svc, fid)
            val tmp = File("/data/local/tmp/$fid.apk")
            try { dest.inputStream().use { i -> tmp.outputStream().use { o -> i.copyTo(o) } } } catch (e: Exception) {
                reportCommandStatus(fid, "failed", "Copy failed"); UpgradingMark.clear(svc); removeSkip(fid); return
            }
            val result = runPmInstall(tmp.absolutePath)
            if (result.startsWith("SUCCESS")) {
                val vc = try { packageManager.getPackageInfo(packageName, 0).longVersionCode } catch (e: Exception) { 0L }
                reportCommandStatus(fid, "completed", "Installed version $vc")
                UpgradingMark.clear(svc)
            } else {
                reportCommandStatus(fid, "failed", result)
                UpgradingMark.clear(svc); removeSkip(fid)
            }
            try { tmp.delete() } catch (e: Exception) {}
            try { dest.delete() } catch (e: Exception) {}
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
            try {
                val j = JSONObject().apply { put("batch_id", batchId); put("file_id", fileId); put("status", status); put("downloaded_bytes", bytes) }
                val c = URL("$cfgServerUrl/api/v1/devices/$cfgDeviceId/release_status").openConnection() as HttpURLConnection
                c.requestMethod = "POST"; c.connectTimeout = 5000; c.readTimeout = 10000
                c.setRequestProperty("Content-Type", "application/json"); c.doOutput = true
                c.outputStream.use { it.write(j.toString().toByteArray()) }; c.responseCode; c.disconnect()
            } catch (e: Exception) {}
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
}

private val skipFile: File get() = File("/data/data/com.deviceagent/files/skip_cmds.txt")
private val inFlightCmds = mutableSetOf<String>()
private val inFlightFiles = mutableSetOf<String>()

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


fun runPmInstall(apkPath: String): String {
    return try {
        val p = Runtime.getRuntime().exec(arrayOf("pm", "install", "-r", apkPath))
        val out = p.inputStream.bufferedReader().readText().trim()
        val err = p.errorStream.bufferedReader().readText().trim()
        val code = p.waitFor()
        android.util.Log.i("DeviceAgentService", "pm install exitCode=$code out=$out err=$err")
        if (code == 0 || out.startsWith("Success")) "SUCCESS" else if (err.isNotEmpty()) err else out
    } catch (e: Exception) { android.util.Log.e("DeviceAgentService", "pm install exception: ${e.message}"); "Exception: ${e.message}" }
}

