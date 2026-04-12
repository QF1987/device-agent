// ============================================================
// service/UIHelper.kt - JNI UI 桥接
// ============================================================
// 提供静态方法供 C++ 通过 JNI 调用显示 UI（Toast / Dialog）。
//
// C++ 层不能直接操作 UI，必须通过 JNI 调用 Java 方法。
// UIHelper 是 C++ 和 Android UI 之间的桥梁。
// ============================================================

package com.deviceagent

import android.app.AlertDialog
import android.content.Context
import android.content.DialogInterface
import android.os.Handler
import android.os.Looper
import android.widget.Toast

object UIHelper {

    private var appContext: Context? = null
    private val mainHandler = Handler(Looper.getMainLooper())

    // ─── 初始化 ─────────────────────────────────────────
    // 由 DeviceAgentService 在 onCreate 时调用，注入 Context

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    // ─── Toast ─────────────────────────────────────────
    // 由 C++ JNI 调用（非 UI 线程调用）

    @JvmStatic
    fun showToast(message: String) {
        val ctx = appContext ?: return
        mainHandler.post {
            try {
                Toast.makeText(ctx, "[device-agent] $message", Toast.LENGTH_SHORT).show()
            } catch (e: Exception) {
                android.util.Log.e("UIHelper", "showToast error: $e")
            }
        }
    }

    // ─── Dialog ─────────────────────────────────────────
    // 确认对话框（显示在主线程）

    @JvmStatic
    fun showDialog(title: String, message: String) {
        val ctx = appContext ?: return
        mainHandler.post {
            try {
                AlertDialog.Builder(ctx)
                    .setTitle(title)
                    .setMessage(message)
                    .setPositiveButton("OK") { dialog, _ ->
                        dialog.dismiss()
                    }
                    .setCancelable(true)
                    .show()
            } catch (e: Exception) {
                android.util.Log.e("UIHelper", "showDialog error: $e")
            }
        }
    }

    // ─── Notification ───────────────────────────────────
    // 显示状态通知（可由 C++ 调用更新）

    @JvmStatic
    fun showNotification(title: String, message: String) {
        val ctx = appContext ?: return
        mainHandler.post {
            try {
                val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE)
                        as android.app.NotificationManager

                if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                    val channel = android.app.NotificationChannel(
                        "device_agent_status",
                        "device-agent Status",
                        android.app.NotificationManager.IMPORTANCE_DEFAULT
                    )
                    nm.createNotificationChannel(channel)
                }

                val notification = android.app.Notification.Builder(ctx, "device_agent_status")
                    .setContentTitle(title)
                    .setContentText(message)
                    .setSmallIcon(R.drawable.ic_notification)
                    .setPriority(android.app.Notification.PRIORITY_DEFAULT)
                    .build()

                nm.notify(System.currentTimeMillis().toInt(), notification)
            } catch (e: Exception) {
                android.util.Log.e("UIHelper", "showNotification error: $e")
            }
        }
    }
}
