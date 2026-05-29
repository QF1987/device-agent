package com.deviceagent.install

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Handler
import android.os.Looper
import androidx.core.content.FileProvider
import com.deviceagent.BuildConfig
import com.deviceagent.R
import java.io.File

/**
 * 普通应用安装（用户可见弹窗确认）
 *
 * 两种实现方式，通过 BuildConfig.NORMAL_INSTALL_METHOD 切换：
 *
 *   action_view (默认):
 *     通知栏触发 → PendingIntent → ACTION_VIEW + FileProvider → MIUI 安装器弹窗
 *     ✅ 绕过 Android 10+ 后台启动 Activity 限制
 *     ✅ MIUI 兼容（XMSF 不拦截）
 *     ❌ 无法获取安装结果回调
 *
 *   session_commit:
 *     PackageInstaller.Session.commit(IntentSender) → 系统弹窗
 *     ✅ 可通过 InstallResultReceiver 获取结果
 *     ❌ MIUI XMSF 静默拦截，弹窗不出现
 */
class NormalInstaller(ctx: Context) : BaseInstaller(ctx) {
    override val mode = "normal"

    private val useActionView = BuildConfig.NORMAL_INSTALL_METHOD == "action_view"

    override fun install(apkFile: File, callback: (InstallResult) -> Unit) {
        if (useActionView) {
            installViaActionView(apkFile, callback)
        } else {
            installViaSessionCommit(apkFile, callback)
        }
    }

    // ─── ACTION_VIEW 方式（通知触发，绕过后台 Activity 限制）──

    private fun installViaActionView(apkFile: File, callback: (InstallResult) -> Unit) {
        try {
            val uri = FileProvider.getUriForFile(
                ctx,
                "${ctx.packageName}.fileprovider",
                apkFile
            )
            val intent = Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, "application/vnd.android.package-archive")
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }

            // Android 10+ 禁止后台 Service 直接 startActivity。
            // 改用通知栏 PendingIntent：用户点击通知 → 系统代发 Intent → 弹安装窗。
            val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

            // 创建通知渠道（Android 8+）
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                val channel = NotificationChannel(
                    "install_channel",
                    "应用更新",
                    NotificationManager.IMPORTANCE_HIGH
                ).apply {
                    description = "设备应用更新通知"
                    setShowBadge(false)
                }
                nm.createNotificationChannel(channel)
            }

            val pending = PendingIntent.getActivity(
                ctx, System.currentTimeMillis().toInt(), intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )

            val notification = if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                Notification.Builder(ctx, "install_channel")
            } else {
                @Suppress("DEPRECATION") Notification.Builder(ctx)
            }.apply {
                setContentTitle("发现新版本")
                setContentText("点击安装 device-agent 更新")
                setSmallIcon(ctx.applicationInfo.icon)
                setAutoCancel(true)
                setContentIntent(pending)
                setPriority(Notification.PRIORITY_HIGH)
                setCategory(Notification.CATEGORY_STATUS)
            }.build()

            nm.notify(INSTALL_NOTIFY_ID, notification)
            android.util.Log.i("NormalInstaller", "Install notification posted (id=$INSTALL_NOTIFY_ID)")

            callback(InstallResult(true, "Notification posted → tap to install"))
        } catch (e: Exception) {
            callback(InstallResult(false, "ACTION_VIEW failed: ${e.message}"))
        }
    }

    companion object {
        private const val INSTALL_NOTIFY_ID = 2001
    }

    // ─── PackageInstaller.Session.commit() 方式 ───────────────

    private fun installViaSessionCommit(apkFile: File, callback: (InstallResult) -> Unit) {
        try {
            val sessionId = createSession()
            val session = packageInstaller.openSession(sessionId)
            session.use { s ->
                writeApkToSession(s, apkFile)
                s.commit(commitIntent(sessionId))
            }
            callback(InstallResult(true, "Session committed (session=$sessionId)", sessionId = sessionId))
        } catch (e: Exception) {
            callback(InstallResult(false, "Session commit failed: ${e.message}"))
        }
    }
}
