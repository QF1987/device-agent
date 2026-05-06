package com.deviceagent

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * 升级后自动重启 Service
 *
 * Android 升级流程：PackageInstaller 安装新 APK → 杀旧进程 → 发 ACTION_MY_PACKAGE_REPLACED
 * 此时 device-agent 已不在运行，需要本 Receiver 重新拉起。
 */
class PackageReplacedReceiver : BroadcastReceiver() {

    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != Intent.ACTION_MY_PACKAGE_REPLACED) return

        android.util.Log.i(TAG, "Package replaced, restarting DeviceAgentService")

        try {
            val serviceIntent = Intent(context, DeviceAgentService::class.java)
            if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.O) {
                context.startForegroundService(serviceIntent)
            } else {
                context.startService(serviceIntent)
            }
            android.util.Log.i(TAG, "DeviceAgentService restart requested")
        } catch (e: Exception) {
            android.util.Log.e(TAG, "Failed to restart service: ${e.message}")
        }
    }

    companion object {
        const val TAG = "PackageReplacedReceiver"
    }
}
