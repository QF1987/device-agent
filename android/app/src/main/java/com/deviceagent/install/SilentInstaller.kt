package com.deviceagent.install

import android.content.Context
import java.io.File

/**
 * 系统权限静默安装
 *
 * 要求：manifest 声明 INSTALL_PACKAGES，app 使用系统签名。
 * commit 后系统静默安装，用户无感知。
 */
class SilentInstaller(ctx: Context) : BaseInstaller(ctx) {
    override val mode = "silent"

    override fun install(apkFile: File, callback: (InstallResult) -> Unit) {
        try {
            val sessionId = createSession()
            val session = packageInstaller.openSession(sessionId)
            session.use { s ->
                writeApkToSession(s, apkFile)
                s.commit(commitIntent(sessionId))
            }
            val vc = getCurrentVersion()
            callback(InstallResult(true, "Session $sessionId committed", vc, sessionId))
        } catch (e: Exception) {
            callback(InstallResult(false, "Silent install: ${e.message}"))
        }
    }
}
