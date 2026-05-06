package com.deviceagent.install

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller

/**
 * 安装完成广播接收器（观察者模式）
 *
 * PackageInstaller 在安装完成后发送系统广播，
 * 本 Receiver 接收并解析结果，分发给 IInstallObserver。
 */
class InstallResultReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -1)
        val sessionId = intent.getIntExtra(PackageInstaller.EXTRA_SESSION_ID, -1)
        val packageName = intent.getStringExtra(PackageInstaller.EXTRA_PACKAGE_NAME)
        val msg = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: ""

        android.util.Log.i(TAG, "Install complete: session=$sessionId status=$status pkg=$packageName msg=$msg")

        val event = when (status) {
            PackageInstaller.STATUS_SUCCESS -> InstallEvent.Succeeded(sessionId, packageName)
            PackageInstaller.STATUS_PENDING_USER_ACTION -> InstallEvent.PendingUser(sessionId)
            PackageInstaller.STATUS_FAILURE_ABORTED -> InstallEvent.Failed(sessionId, "User aborted")
            PackageInstaller.STATUS_FAILURE_BLOCKED -> InstallEvent.Failed(sessionId, "Blocked by policy")
            PackageInstaller.STATUS_FAILURE_CONFLICT -> InstallEvent.Failed(sessionId, "Package conflict")
            PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> InstallEvent.Failed(sessionId, "Incompatible")
            PackageInstaller.STATUS_FAILURE_INVALID -> InstallEvent.Failed(sessionId, "Invalid APK")
            PackageInstaller.STATUS_FAILURE_STORAGE -> InstallEvent.Failed(sessionId, "Storage error")
            else -> InstallEvent.Failed(sessionId, "$msg (code=$status)")
        }

        InstallEventBus.notify(event)
    }

    companion object {
        const val TAG = "InstallResultReceiver"

        /** 创建用于 commit 的 PendingIntent */
        fun createPendingIntent(ctx: Context, sessionId: Int): PendingIntent {
            val intent = Intent(ctx, InstallResultReceiver::class.java).apply {
                action = ACTION
                putExtra(PackageInstaller.EXTRA_SESSION_ID, sessionId)
            }
            return PendingIntent.getBroadcast(
                ctx, sessionId, intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
            )
        }

        const val ACTION = "com.deviceagent.INSTALL_COMPLETE"
    }
}

/**
 * 安装事件（密封类，确保分支穷尽）
 */
sealed class InstallEvent {
    abstract val sessionId: Int

    data class Succeeded(
        override val sessionId: Int,
        val packageName: String?
    ) : InstallEvent()

    data class Failed(
        override val sessionId: Int,
        val error: String
    ) : InstallEvent()

    data class PendingUser(
        override val sessionId: Int
    ) : InstallEvent()
}

/**
 * 安装事件总线（观察者模式）
 *
 * 多个观察者可以订阅安装事件：
 *   - DeviceAgentService 订阅 → 上报给 serve
 *   - UIHelper 订阅 → 显示 Toast
 */
object InstallEventBus {

    private val observers = mutableListOf<(InstallEvent) -> Unit>()

    fun subscribe(observer: (InstallEvent) -> Unit) {
        synchronized(observers) { observers.add(observer) }
    }

    fun unsubscribe(observer: (InstallEvent) -> Unit) {
        synchronized(observers) { observers.remove(observer) }
    }

    fun notify(event: InstallEvent) {
        val snapshot = synchronized(observers) { observers.toList() }
        snapshot.forEach { observer ->
            try { observer(event) } catch (e: Exception) {
                android.util.Log.e("InstallEventBus", "Observer failed: ${e.message}")
            }
        }
    }
}
