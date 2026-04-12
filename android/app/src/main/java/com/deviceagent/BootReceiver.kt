package com.deviceagent

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

// 设备启动后自动启动 device-agent Service
class BootReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "BootReceiver"
    }

    override fun onReceive(context: Context?, intent: Intent?) {
        if (intent?.action == Intent.ACTION_BOOT_COMPLETED) {
            Log.i(TAG, "Boot completed, starting DeviceAgentService")
            val serviceIntent = Intent(context, DeviceAgentService::class.java)
            context?.startForegroundService(serviceIntent)
        }
    }
}
