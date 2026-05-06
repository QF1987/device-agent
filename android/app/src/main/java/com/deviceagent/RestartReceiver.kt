package com.deviceagent

import android.app.AlarmManager
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build

/**
 * 升级后自动重启 Service（AlarmManager + 双重闹钟方案）
 *
 * 闹钟 1（RestartReceiver 广播）：30 秒检查一次，版本比对 + 续约
 * 闹钟 2（ForegroundService）：检测到升级后设 1 秒闹钟，由 AlarmManager 直接拉起 Service
 *
 * 为什么不用 pi.send()：PendingIntent.send() 沿用当前广播接收器的 callingContext，
 * Android 12+ 仍然会拦截 "Background start not allowed"。
 */
class RestartReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_RESTART) return

        val pendingFile = java.io.File(context.filesDir, "pending_upgrade")
        if (pendingFile.exists()) {
            try {
                val oldVersion = pendingFile.readText().trim().toLongOrNull() ?: 0
                val curVersion = context.packageManager.getPackageInfo(context.packageName, 0).longVersionCode
                if (curVersion != oldVersion) {
                    // 升级完成 → 设 1 秒闹钟，AlarmManager 直接发 FgService intent
                    android.util.Log.i(TAG, "Upgrade detected: $oldVersion → $curVersion, scheduling fg-launch alarm")
                    pendingFile.delete()
                    scheduleFgLaunch(context)
                    return  // 不续约 check 闹钟
                } else {
                    android.util.Log.i(TAG, "Version unchanged ($curVersion), rescheduling alarm")
                    scheduleCheck(context)
                    return
                }
            } catch (e: Exception) {
                android.util.Log.w(TAG, "Failed to check version: ${e.message}, rescheduling")
                scheduleCheck(context)
                return
            }
        }
        // pending_upgrade 已删 → 升级已完成，取消残留闹钟
        android.util.Log.i(TAG, "No pending upgrade, cancelling check alarm")
        cancelCheck(context)
    }

    companion object {
        const val TAG = "RestartReceiver"
        const val ACTION_RESTART = "com.deviceagent.action.RESTART"
        private const val CHECK_REQUEST_CODE = 9001

        // ─── 给 handleDownloadAndInstall / self-upgrade 调用 ───

        /** 安装前调用：设检查闹钟（升级检测后会自动设 FgLaunch 闹钟） */
        fun scheduleRestart(context: Context) {
            scheduleCheck(context)
        }

        /** Service 启动后调用：取消所有残留闹钟 */
        fun cancelRestart(context: Context) {
            cancelCheck(context)
            cancelFgLaunch(context)
        }

        // ─── 内部实现 ───

        /** 闹钟 1：30 秒后触发 RestartReceiver 检查版本 */
        private fun scheduleCheck(context: Context) {
            val alarmManager = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            val intent = Intent(context, RestartReceiver::class.java).apply {
                action = ACTION_RESTART
            }
            val pi = PendingIntent.getBroadcast(
                context, CHECK_REQUEST_CODE, intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            scheduleExact(alarmManager, System.currentTimeMillis() + 30_000L, pi, "Check")
        }

        private fun cancelCheck(context: Context) {
            val alarmManager = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            val intent = Intent(context, RestartReceiver::class.java).apply {
                action = ACTION_RESTART
            }
            val pi = PendingIntent.getBroadcast(
                context, CHECK_REQUEST_CODE, intent,
                PendingIntent.FLAG_NO_CREATE or PendingIntent.FLAG_IMMUTABLE
            ) ?: return
            alarmManager.cancel(pi)
        }

        /** 闹钟 2：1 秒后由 AlarmManager 直接拉起 DeviceAgentService（绕过广播限制） */
        private fun scheduleFgLaunch(context: Context) {
            val alarmManager = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            val intent = Intent(context, DeviceAgentService::class.java)
            val pi = PendingIntent.getForegroundService(
                context, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
            // 短延迟，让 check 闹钟先执行
            scheduleExact(alarmManager, System.currentTimeMillis() + 1_000L, pi, "FgLaunch")
        }

        private fun cancelFgLaunch(context: Context) {
            val alarmManager = context.getSystemService(Context.ALARM_SERVICE) as AlarmManager
            val intent = Intent(context, DeviceAgentService::class.java)
            val pi = PendingIntent.getForegroundService(
                context, 0, intent,
                PendingIntent.FLAG_NO_CREATE or PendingIntent.FLAG_IMMUTABLE
            ) ?: return
            alarmManager.cancel(pi)
        }

        // ─── 闹钟调度辅助（降级链：alarmClock → setExact → setWindow）───

        private fun scheduleExact(am: AlarmManager, triggerTime: Long, pi: PendingIntent, label: String) {
            try {
                am.setAlarmClock(AlarmManager.AlarmClockInfo(triggerTime, pi), pi)
                android.util.Log.i(TAG, "$label alarmClock scheduled")
            } catch (e: SecurityException) {
                try {
                    am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, triggerTime, pi)
                    android.util.Log.i(TAG, "$label exact alarm scheduled")
                } catch (e2: SecurityException) {
                    android.util.Log.w(TAG, "$label: no exact permission, using setWindow")
                    am.setWindow(AlarmManager.RTC_WAKEUP, triggerTime, 30_000L, pi)
                }
            } catch (e: Exception) {
                android.util.Log.w(TAG, "$label alarmClock error: ${e.message}, using setWindow")
                am.setWindow(AlarmManager.RTC_WAKEUP, triggerTime, 30_000L, pi)
            }
        }
    }
}
