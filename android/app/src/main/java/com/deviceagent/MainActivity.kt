package com.deviceagent

import android.app.Activity
import android.os.Bundle
import android.widget.TextView

// 简单的调试 Activity（可选）
class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val tv = TextView(this).apply {
            text = "device-agent is running.\nSee notification for status."
        }
        setContentView(tv)
    }
}
