package com.deviceagent

import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Toast

/**
 * 透明启动页：启动后直接拉起 Service，然后立即销毁自己。
 * 目的：让 BootReceiver / am start 等场景能触发 Service 启动，
 * 同时避免在每次启动时都弹 UI 窗口。
 */
class MainActivity : Activity() {
    companion object {
        private const val TAG = "MainActivity"
        private const val REQUEST_NOTIFICATION = 1001
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val caller = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            callingActivity?.className ?: "system"
        } else {
            @Suppress("DEPRECATION")
            callingActivity?.className ?: "system"
        }
        android.util.Log.i(TAG, "onCreate called by: $caller, intent: ${intent?.action ?: "none"}")

        // Android 13+ 需要请求通知权限
        if (Build.VERSION.SDK_INT >= 33) {
            if (checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), REQUEST_NOTIFICATION)
                // 权限请求是异步的，不在这里 startService，等回调再启动
                return
            }
        }

        startAndFinish()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        android.util.Log.i(TAG, "Notification permission result: ${grantResults.firstOrNull()}")
        startAndFinish()
    }

    private fun startAndFinish() {
        if (!refreshRunningDeviceAgentService()) {
            val serviceIntent = Intent(this, DeviceAgentService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(serviceIntent)
            } else {
                @Suppress("DEPRECATION")
                startService(serviceIntent)
            }
        }
        Toast.makeText(this, "device-agent 已启动", Toast.LENGTH_SHORT).show()
        window.decorView.postDelayed({
            finish()
        }, 2_000)
    }
}
