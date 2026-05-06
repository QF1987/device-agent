package com.deviceagent.install

import android.content.Context
import java.io.File

/**
 * 定制 ROM 安装
 *
 * 适用场景：设备运行定制 Android 系统，厂商暴露了程序化安装接口。
 * 当前为空实现，待厂商对接后填充。
 */
class CustomInstaller(ctx: Context) : BaseInstaller(ctx) {
    override val mode = "custom_rom"

    override fun install(apkFile: File, callback: (InstallResult) -> Unit) {
        // TODO: 调用厂商暴露的安装接口
        // 例如：Runtime.exec("/system/bin/vendor_install ${apkFile.absolutePath}")
        // 或通过 JNI 回调定制系统服务
        callback(InstallResult(false, "CustomRomInstaller: not implemented yet"))
    }
}
