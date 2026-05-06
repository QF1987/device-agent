package com.deviceagent.install

import java.io.File

/**
 * 安装结果
 */
data class InstallResult(
    val success: Boolean,
    val message: String,
    val versionCode: Long = 0,
    val sessionId: Int = -1   // PackageInstaller session id，Silent/Normal 有效
)

/**
 * 应用安装器接口（策略模式）
 *
 * 三种实现：
 *   - SilentInstaller   : 系统权限静默安装（INSTALL_PACKAGES + PackageInstaller）
 *   - CustomInstaller    : 定制 ROM 暴露的安装接口
 *   - NormalInstaller    : 普通应用升级（PackageInstaller + 用户确认弹窗）
 */
interface IAppInstaller {

    /** 安装方式名称 */
    val mode: String

    /**
     * 安装 APK 文件
     *
     * @param apkFile  已下载且校验通过的 APK 文件
     * @param callback 安装结果回调（主线程或非主线程皆可）
     */
    fun install(apkFile: File, callback: (InstallResult) -> Unit)
}
