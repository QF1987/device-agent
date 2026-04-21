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
        CommandPoller.start()
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
                    val result = CommandPoller.runPmInstall(apkFile.absolutePath)
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
    override fun onDestroy() { CommandPoller.stop(); mainServiceRef = null; super.onDestroy() }
    private fun mkChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "device-agent", NotificationManager.IMPORTANCE_LOW))
    }
    private fun mkNotification(text: String): Notification {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            Notification.Builder(this, CHANNEL_ID).setContentTitle("device-agent").setContentText(text)
                .setSmallIcon(R.drawable.ic_notification).setPriority(Notification.PRIORITY_LOW).build()
        else
            @Suppress("DEPRECATION")
            Notification.Builder(this).setContentTitle("device-agent").setContentText(text)
                .setSmallIcon(R.drawable.ic_notification).setPriority(Notification.PRIORITY_LOW).build()
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

object CommandPoller {
    private const val TAG = "CommandPoller"
    private const val POLL_MS = 10_000L
    private const val CONN_TO = 5_000
    private const val READ_TO = 30_000
    @Volatile private var running = false
    private var thread: Thread? = null
    fun start() {
        if (running) return
        running = true
        android.util.Log.i(TAG, "CommandPoller start")
        thread = Thread {
            while (running) {
                try { poll() } catch (e: Exception) { android.util.Log.w(TAG, "Poll error: ${e.message}") }
                try { Thread.sleep(POLL_MS) } catch (e: InterruptedException) { break }
            }
        }.apply { start() }
    }
    fun stop() { running = false; thread?.interrupt(); thread = null }
    private fun poll() {
        val conn = URL("$cfgServerUrl/api/v1/devices/$cfgDeviceId/poll").openConnection() as HttpURLConnection
        conn.requestMethod = "GET"; conn.connectTimeout = CONN_TO; conn.readTimeout = READ_TO
        conn.setRequestProperty("Accept", "application/json")
        if (conn.responseCode != 200) { conn.disconnect(); return }
        val body = conn.inputStream.bufferedReader().readText(); conn.disconnect()
        android.util.Log.d(TAG, "Poll response: $body")
        val cmds = JSONObject(body).optJSONArray("commands") ?: return
        for (i in 0 until cmds.length()) {
            val c = cmds.getJSONObject(i)
            android.util.Log.i(TAG, "cmd: ${c.getString("command_type")} id=${c.getString("command_id")}")
            handleCmd(c.getString("command_id"), c.getString("command_type").uppercase(), c.optString("payload_json", "{}"))
        }
    }
    private fun handleCmd(cmdId: String, type: String, payloadJson: String) {
        if (markSkipped(cmdId)) { android.util.Log.i(TAG, "Skip $cmdId"); return }
        val svc = mainServiceRef?.get() ?: return
        if ((type == "DOWNLOAD_READY" || type == "UPGRADE_APP" || type == "CHECK_LOCAL_APK") && UpgradingMark.isUpgrading(svc)) return
        val p = JSONObject(payloadJson)
        when (type) {
            "DOWNLOAD_READY" -> handleDL(cmdId, p, svc)
            "UPGRADE_APP" -> handleUP(cmdId, p, svc)
            "CHECK_LOCAL_APK" -> handleCheckLocalApk(cmdId, p, svc)
        }
    }
    private fun handleCheckLocalApk(cmdId: String, p: JSONObject, svc: Service) {
        // 优先使用 app 私有外部存储路径：/sdcard/Android/data/<pkg>/files/DeviceAgent/Apk/
        // 这个路径不需要任何权限，app 天然有完整访问权限
        // 如果没有外部存储，则回退到 filesDir（/data/data/<pkg>/files/）
        val customPath = p.optString("path", "")
        val dir: File = if (customPath.isNotEmpty()) {
            File(customPath)
        } else {
            // 自动检测最优路径
            val extDir = svc.getExternalFilesDir("DeviceAgent/Apk")
            if (extDir != null) extDir else File(svc.filesDir, "DeviceAgent/Apk")
        }

        // 自动创建目录（如果不存在）
        if (!dir.exists()) {
            android.util.Log.i(TAG, "Creating upgrade directory: ${dir.absolutePath}")
            if (!dir.mkdirs()) {
                android.util.Log.e(TAG, "Failed to create directory: ${dir.absolutePath}")
                reportCS(cmdId, "failed", "Cannot create directory: ${dir.absolutePath}")
                return
            }
        }

        android.util.Log.i(TAG, "CHECK_LOCAL_APK scanning: ${dir.absolutePath}")
        val apks = dir.listFiles { _, name -> name.endsWith(".apk") }
        if (apks == null || apks.isEmpty()) {
            android.util.Log.i(TAG, "No APK found in ${dir.absolutePath}")
            reportCS(cmdId, "failed", "No APK found in ${dir.absolutePath}")
            return
        }
        val apkFile = apks.maxByOrNull { it.lastModified() }!!
        android.util.Log.i(TAG, "Found APK: ${apkFile.absolutePath}, launching install intent (MIUI will prompt)")
        UpgradingMark.write(svc, cmdId)
        reportCS(cmdId, "installing", "MIUI confirmation required for ${apkFile.name}")
        try {
            val uri = androidx.core.content.FileProvider.getUriForFile(svc, "${svc.packageName}.fileprovider", apkFile)
            svc.startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION))
            android.util.Log.i(TAG, "Install intent launched, waiting for MIUI confirmation...")
            Thread {
                waitInstallResult(cmdId, apkFile, svc)
            }.start()
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Install intent failed: ${e.message}")
            reportCS(cmdId, "failed", e.message ?: "Install failed")
            UpgradingMark.clear(svc)
        }
    }
    private fun waitInstallResult(cmdId: String, apkFile: File, svc: Service) {
        val expected = try { svc.packageManager.getPackageArchiveInfo(apkFile.absolutePath, 0)?.longVersionCode ?: 0L } catch (e: Exception) { 0L }
        val before = try { svc.packageManager.getPackageInfo(svc.packageName, 0).longVersionCode } catch (e: Exception) { 0L }
        android.util.Log.i(TAG, "waitInstallResult expected=$expected before=$before")
        var n = 0
        while (n < 30) {
            Thread.sleep(2000); n++
            try {
                val vc = svc.packageManager.getPackageInfo(svc.packageName, 0).longVersionCode
                android.util.Log.i(TAG, "versionCode=$vc (expected=$expected)")
                if (vc == expected || vc > before) {
                    reportCS(cmdId, "completed", "Installed version $vc")
                    UpgradingMark.clear(svc)
                    android.util.Log.i(TAG, "Install SUCCESS")
                    return
                }
            } catch (e: android.content.pm.PackageManager.NameNotFoundException) {
                android.util.Log.d(TAG, "waitInstallResult attempt $n/30")
            }
        }
        android.util.Log.e(TAG, "Install timeout")
        reportCS(cmdId, "failed", "Install timeout (expected $expected)")
        UpgradingMark.clear(svc)
        removeSkip(cmdId)
    }
    private fun handleDL(cmdId: String, p: JSONObject, svc: Service) {
        val batchId = p.optString("batch_id", ""); val fileId = p.getString("file_id")
        val url = p.getString("download_url"); val sha = p.optString("sha256", ""); val size = p.optLong("file_size", 0)
        android.util.Log.i(TAG, "download_ready: batch=$batchId file=$fileId size=$size")
        synchronized(inFlightFiles) { if (inFlightFiles.contains(fileId)) return; inFlightFiles.add(fileId) }
        Thread {
            val dest = File(File(svc.filesDir, "downloads").also { it.mkdirs() }, "$fileId.apk")
            reportRS(batchId, fileId, "downloading")
            if (!download(url, dest, sha)) {
                synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
                reportCS(cmdId, "failed", "Download failed"); reportRS(batchId, fileId, "download_failed"); return@Thread
            }
            synchronized(inFlightFiles) { inFlightFiles.remove(fileId) }
            reportRS(batchId, fileId, "downloaded", size)
            android.util.Log.i(TAG, "Download OK, running pm install")
            UpgradingMark.write(svc, cmdId)
            reportCS(cmdId, "installing", "Copying APK")
            reportRS(batchId, fileId, "installing")
            val tmp = File("/data/local/tmp/$fileId.apk")
            try { dest.inputStream().use { i -> tmp.outputStream().use { o -> i.copyTo(o) } } } catch (e: Exception) {
                android.util.Log.e(TAG, "Copy failed: ${e.message}"); reportCS(cmdId, "failed", "Copy failed")
                reportRS(batchId, fileId, "install_failed"); UpgradingMark.clear(svc); removeSkip(cmdId); return@Thread
            }
            reportCS(cmdId, "installing", "Running pm install")
            val result = runPmInstall(tmp.absolutePath)
            android.util.Log.i(TAG, "pm install result: $result")
            if (result.startsWith("SUCCESS")) {
                val vc = try { svc.packageManager.getPackageInfo(svc.packageName, 0).longVersionCode } catch (e: Exception) { 0L }
                reportCS(cmdId, "completed", "Installed version $vc"); reportRS(batchId, fileId, "installed")
                UpgradingMark.clear(svc)
                try { tmp.delete() } catch (e: Exception) {}; try { dest.delete() } catch (e: Exception) {}
            } else {
                android.util.Log.e(TAG, "pm install failed: $result"); reportCS(cmdId, "failed", result)
                reportRS(batchId, fileId, "install_failed"); UpgradingMark.clear(svc); removeSkip(cmdId)
                try { tmp.delete() } catch (e: Exception) {}
            }
        }.start()
    }
    private fun handleUP(cmdId: String, p: JSONObject, svc: Service) {
        val url = p.optString("download_url", ""); val md5 = p.optString("md5", ""); val apk = p.optString("apk_path", "")
        val fid = "upgrade_${System.currentTimeMillis()}"
        when {
            url.isNotEmpty() -> {
                Thread {
                    val dest = File(File(svc.filesDir, "downloads").also { it.mkdirs() }, "$fid.apk")
                    if (!downloadMD5(url, dest, md5)) { reportCS(cmdId, "failed", "Download failed"); return@Thread }
                    UpgradingMark.write(svc, cmdId)
                    val tmp = File("/data/local/tmp/$fid.apk")
                    try { dest.inputStream().use { i -> tmp.outputStream().use { o -> i.copyTo(o) } } } catch (e: Exception) {
                        reportCS(cmdId, "failed", "Copy failed"); UpgradingMark.clear(svc); removeSkip(cmdId); return@Thread
                    }
                    val result = runPmInstall(tmp.absolutePath)
                    if (result.startsWith("SUCCESS")) {
                        val vc = try { svc.packageManager.getPackageInfo(svc.packageName, 0).longVersionCode } catch (e: Exception) { 0L }
                        reportCS(cmdId, "completed", "Installed version $vc"); UpgradingMark.clear(svc)
                    } else { reportCS(cmdId, "failed", result); UpgradingMark.clear(svc); removeSkip(cmdId) }
                    try { tmp.delete() } catch (e: Exception) {}; try { dest.delete() } catch (e: Exception) {}
                }.start()
            }
            apk.isNotEmpty() -> {
                UpgradingMark.write(svc, cmdId)
                val result = runPmInstall(apk)
                if (result.startsWith("SUCCESS")) {
                    val vc = try { svc.packageManager.getPackageInfo(svc.packageName, 0).longVersionCode } catch (e: Exception) { 0L }
                    reportCS(cmdId, "completed", "Installed version $vc"); UpgradingMark.clear(svc)
                } else { reportCS(cmdId, "failed", result); UpgradingMark.clear(svc); removeSkip(cmdId) }
            }
        }
    }
    private fun download(url: String, dest: File, sha: String): Boolean {
        var conn: HttpURLConnection? = null
        return try {
            conn = URL(url).openConnection() as HttpURLConnection
            conn.requestMethod = "GET"; conn.connectTimeout = CONN_TO; conn.readTimeout = READ_TO
            conn.setRequestProperty("Accept", "application/octet-stream")
            if (conn.responseCode != 200 && conn.responseCode != 206) { conn.disconnect(); return false }
            val d = MessageDigest.getInstance("SHA-256")
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
    private fun downloadMD5(url: String, dest: File, md5: String): Boolean {
        var conn: HttpURLConnection? = null
        return try {
            conn = URL(url).openConnection() as HttpURLConnection
            conn.requestMethod = "GET"; conn.connectTimeout = CONN_TO; conn.readTimeout = READ_TO
            conn.setRequestProperty("Accept", "application/octet-stream")
            if (conn.responseCode != 200 && conn.responseCode != 206) { conn.disconnect(); return false }
            val d = MessageDigest.getInstance("MD5")
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
    private fun reportRS(batchId: String, fileId: String, status: String, bytes: Long = 0) {
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
    private fun reportCS(cmdId: String, status: String, msg: String) {
        try {
            val j = JSONObject().apply { put("command_id", cmdId); put("status", status); put("result_message", msg) }
            val c = URL("$cfgServerUrl/api/v1/devices/$cfgDeviceId/report").openConnection() as HttpURLConnection
            c.requestMethod = "POST"; c.connectTimeout = 5000; c.readTimeout = 10000
            c.setRequestProperty("Content-Type", "application/json"); c.doOutput = true
            c.outputStream.use { it.write(j.toString().toByteArray()) }
            val httpCode = c.responseCode
            android.util.Log.i(TAG, "Report: $cmdId status=$status httpCode=$httpCode")
            c.disconnect()
        } catch (e: Exception) { android.util.Log.e(TAG, "Report failed: ${e.message}") }
    }
    fun runPmInstall(apkPath: String): String {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("pm", "install", "-r", apkPath))
            val out = p.inputStream.bufferedReader().readText().trim()
            val err = p.errorStream.bufferedReader().readText().trim()
            val code = p.waitFor()
            android.util.Log.i(TAG, "pm install exitCode=$code out=$out err=$err")
            if (code == 0 || out.startsWith("Success")) "SUCCESS" else if (err.isNotEmpty()) err else out
        } catch (e: Exception) { android.util.Log.e(TAG, "pm install exception: ${e.message}"); "Exception: ${e.message}" }
    }
}

class UpgradeService : Service() {
    companion object { private const val TAG = "UpgradeService"; private const val NID = 1002; private const val CID = "upgrade_channel" }
    override fun onCreate() {
        super.onCreate()
        android.util.Log.i(TAG, "UpgradeService onCreate")
        UpgradingMark.clear(this)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            getSystemService(NotificationManager::class.java).createNotificationChannel(
                NotificationChannel(CID, "app-upgrade", NotificationManager.IMPORTANCE_LOW))
        }
        startForeground(NID, Notification.Builder(this, CID).setContentTitle("Upgrading").setContentText("Installing update...")
            .setSmallIcon(R.drawable.ic_notification).setPriority(Notification.PRIORITY_LOW).build())
    }
    override fun onStartCommand(i: Intent?, f: Int, s: Int): Int {
        val cmdId = i?.getStringExtra("command_id") ?: ""; val apk = i?.getStringExtra("apk_path") ?: ""
        val batchId = i?.getStringExtra("batch_id") ?: ""; val fileId = i?.getStringExtra("file_id") ?: ""
        android.util.Log.i(TAG, "UpgradeService cmdId=$cmdId apk=$apk")
        if (apk.isEmpty()) { stopSelf(); return START_NOT_STICKY }
        Thread { install(File(apk), cmdId, batchId, fileId) }.start()
        return START_NOT_STICKY
    }
    override fun onBind(i: Intent?): IBinder? = null
    private fun install(f: File, cmdId: String, batchId: String, fileId: String) {
        if (!f.exists()) { onFail(cmdId, batchId, fileId, "APK not found"); return }
        val expected = try { packageManager.getPackageArchiveInfo(f.absolutePath, 0)?.longVersionCode ?: 0L } catch (e: Exception) { 0L }
        val before = try { packageManager.getPackageInfo(packageName, 0).longVersionCode } catch (e: Exception) { 0L }
        android.util.Log.i(TAG, "Expected=$expected before=$before")
        try {
            val uri = androidx.core.content.FileProvider.getUriForFile(this, "$packageName.fileprovider", f)
            startActivity(android.content.Intent(android.content.Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION))
            android.util.Log.i(TAG, "Install intent launched")
            reportCS(cmdId, "installing", "Install started")
            waitInstall(cmdId, batchId, fileId, expected, before)
        } catch (e: Exception) { android.util.Log.e(TAG, "Install error: ${e.message}"); onFail(cmdId, batchId, fileId, e.message ?: "error") }
    }
    private fun waitInstall(cmdId: String, batchId: String, fileId: String, expected: Long, before: Long) {
        var n = 0
        while (n < 30) {
            Thread.sleep(2000); n++
            try {
                val vc = packageManager.getPackageInfo(packageName, 0).longVersionCode
                android.util.Log.i(TAG, "versionCode=$vc (expected=$expected)")
                if (vc == expected || vc > before) { onSucc(cmdId, batchId, fileId, vc); return }
            } catch (e: android.content.pm.PackageManager.NameNotFoundException) { android.util.Log.d(TAG, "attempt $n/30") }
        }
        android.util.Log.e(TAG, "Install timeout"); onFail(cmdId, batchId, fileId, "Install timeout (expected $expected)")
    }
    private fun onSucc(cmdId: String, batchId: String, fileId: String, vc: Long) {
        reportCS(cmdId, "completed", "Installed version $vc"); reportRS(batchId, fileId, "installed")
        UpgradingMark.clear(this); stopSelf()
    }
    private fun onFail(cmdId: String, batchId: String, fileId: String, reason: String) {
        reportCS(cmdId, "failed", reason); reportRS(batchId, fileId, "install_failed")
        UpgradingMark.clear(this); removeSkip(cmdId); stopSelf()
    }
    private fun reportCS(cmdId: String, status: String, msg: String) {
        try {
            val j = JSONObject().apply { put("command_id", cmdId); put("status", status); put("result_message", msg) }
            val c = URL("$cfgServerUrl/api/v1/devices/ANDROID-001/report").openConnection() as HttpURLConnection
            c.requestMethod = "POST"; c.connectTimeout = 5000; c.readTimeout = 10000
            c.setRequestProperty("Content-Type", "application/json"); c.doOutput = true
            c.outputStream.use { it.write(j.toString().toByteArray()) }
            val httpCode = c.responseCode
            android.util.Log.i(TAG, "Report: $cmdId status=$status httpCode=$httpCode")
            c.disconnect()
        } catch (e: Exception) { android.util.Log.e(TAG, "Report failed: ${e.message}") }
    }
    private fun reportRS(batchId: String, fileId: String, status: String) {
        if (batchId.isEmpty()) return
        try {
            val j = JSONObject().apply { put("batch_id", batchId); put("file_id", fileId); put("status", status) }
            val c = URL("$cfgServerUrl/api/v1/devices/ANDROID-001/release_status").openConnection() as HttpURLConnection
            c.requestMethod = "POST"; c.connectTimeout = 5000; c.readTimeout = 10000
            c.setRequestProperty("Content-Type", "application/json"); c.doOutput = true
            c.outputStream.use { it.write(j.toString().toByteArray()) }; c.responseCode; c.disconnect()
        } catch (e: Exception) {}
    }
}
