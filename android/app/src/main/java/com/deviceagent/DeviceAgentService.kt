// ============================================================
// service/DeviceAgentService.kt - Android Service
// ============================================================
// Android Service，负责：
//   1. 管理 C++ daemon 生命周期（启动/停止/重启）
//   2. Watchdog 监控子进程是否存活
//   3. 处理 JNI 回调（显示 Toast / Dialog / Notification）
//   4. 通过 Unix Socket 与 C++ daemon 通信
//
// C++ daemon 作为子进程运行，Java Service 作为守护进程管理它。
// ============================================================

package com.deviceagent

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.Process
import android.widget.Toast
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class DeviceAgentService : Service() {

    companion object {
        private const val TAG = "DeviceAgentService"
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID = "device_agent_channel"

        // C++ daemon 进程
        private var daemonProcess: Process? = null
        private var watchdogExecutor = Executors.newSingleThreadScheduledExecutor()

        // Unix Socket 端口（C++ daemon 监听）
        private const val DAEMON_SOCKET_PORT = 18998
    }

    // ─── 生命周期 ─────────────────────────────────────────

    override fun onCreate() {
        super.onCreate()
        android.util.Log.i(TAG, "DeviceAgentService onCreate")

        createNotificationChannel()
        startForeground(NOTIFICATION_ID, createNotification("device-agent starting..."))

        // 启动 C++ daemon
        startDaemon()

        // 启动 Watchdog
        startWatchdog()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        android.util.Log.i(TAG, "DeviceAgentService onStartCommand")
        return START_STICKY  // 系统杀死后会自动重启
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        android.util.Log.i(TAG, "DeviceAgentService onDestroy")
        watchdogExecutor.shutdown()
        stopDaemon()
        super.onDestroy()
    }

    // ─── Daemon 管理 ─────────────────────────────────────

    private fun startDaemon() {
        android.util.Log.i(TAG, "Starting C++ daemon...")

        try {
            // C++ daemon 二进制路径（从 assets 或 /data/local/tmp/ 解压）
            val daemonPath = findDaemonBinary()
            if (daemonPath == null) {
                android.util.Log.e(TAG, "Daemon binary not found!")
                showToast("device-agent: daemon binary not found")
                return
            }

            // 启动 C++ daemon 作为子进程
            val pb = ProcessBuilder(daemonPath)
            pb.environment()["DEVICE_AGENT_ANDROID"] = "1"
            pb.redirectErrorStream(true)

            daemonProcess = pb.start()
            android.util.Log.i(TAG, "Daemon started, pid=${daemonProcess?.pid}")

            // 读取 daemon 输出（防止 buffer 满而阻塞）
            Thread {
                val reader = BufferedReader(InputStreamReader(daemonProcess!!.inputStream))
                var line: String?
                while (reader.readLine().also { line = it } != null) {
                    android.util.Log.d(TAG, "[daemon] $line")
                }
            }.start()

            // 等待 daemon 退出
            Thread {
                try {
                    daemonProcess?.waitFor()
                    android.util.Log.w(TAG, "Daemon exited with code=${daemonProcess?.exitValue()}")
                } catch (e: Exception) {
                    android.util.Log.e(TAG, "Daemon waitFor error: $e")
                }
            }.start()

            showToast("device-agent started")

        } catch (e: Exception) {
            android.util.Log.e(TAG, "Failed to start daemon: $e")
            showToast("device-agent: start failed")
        }
    }

    private fun stopDaemon() {
        android.util.Log.i(TAG, "Stopping C++ daemon...")
        daemonProcess?.let { proc ->
            proc.destroy()
            try {
                proc.waitFor(5, TimeUnit.SECONDS)
            } catch (e: Exception) {
                proc.destroyForcibly()
            }
        }
        daemonProcess = null
        android.util.Log.i(TAG, "Daemon stopped")
    }

    private fun findDaemonBinary(): String? {
        // 优先从 /data/local/tmp/ 找（C++ daemon 编译产物）
        val path = "/data/local/tmp/device-agent"
        return if (File(path).canExecute()) path else null
    }

    // ─── Watchdog ─────────────────────────────────────────

    private fun startWatchdog() {
        watchdogExecutor.scheduleAtFixedRate({
            try {
                val proc = daemonProcess
                if (proc == null || !proc.isAlive) {
                    android.util.Log.w(TAG, "Watchdog: daemon not alive, restarting...")
                    showToast("device-agent: daemon crashed, restarting...")
                    startDaemon()
                }

                // 检查 daemon 是否还在响应（检查心跳文件）
                val heartbeatFile = File("/data/local/tmp/device-agent-heartbeat")
                if (heartbeatFile.exists()) {
                    val lastBeat = heartbeatFile.readText().trim().toLongOrNull()
                    val now = System.currentTimeMillis()
                    if (lastBeat != null && now - lastBeat > 60_000) {
                        android.util.Log.w(TAG, "Watchdog: heartbeat stale (${now - lastBeat}ms), restarting daemon")
                        stopDaemon()
                        startDaemon()
                    }
                }
            } catch (e: Exception) {
                android.util.Log.e(TAG, "Watchdog error: $e")
            }
        }, 10, 10, TimeUnit.SECONDS)  // 每 10 秒检查一次
    }

    // ─── Notification ────────────────────────────────────

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "device-agent",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "device-agent daemon status"
            }
            val nm = getSystemService(NotificationManager::class.java)
            nm.createNotificationChannel(channel)
        }
    }

    private fun createNotification(text: String): Notification {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, CHANNEL_ID)
                .setContentTitle("device-agent")
                .setContentText(text)
                .setSmallIcon(R.drawable.ic_notification)
                .setPriority(Notification.PRIORITY_LOW)
                .build()
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
                .setContentTitle("device-agent")
                .setContentText(text)
                .setSmallIcon(R.drawable.ic_notification)
                .setPriority(Notification.PRIORITY_LOW)
                .build()
        }
    }

    // ─── JNI 回调 ─────────────────────────────────────────

    // 由 JNI 调用：显示 Toast
    fun staticShowToast(msg: String) {
        showToast(msg)
    }

    private fun showToast(msg: String) {
        try {
            Toast.makeText(this, "[device-agent] $msg", Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Toast error: $e")
        }
    }
}
