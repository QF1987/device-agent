package com.deviceagent.install

import android.content.Context
import android.content.pm.PackageInstaller
import java.io.File
import java.io.FileInputStream

/**
 * 安装器基类（模板方法模式的基础）
 *
 * 封装 PackageInstaller Session 的公共操作：
 *   - 创建 Session
 *   - 流式写入 APK
 *   - 查询当前版本
 *
 * commit 通过 InstallResultReceiver 的 PendingIntent 完成，
 * 安装结果由 InstallEventBus 分发给观察者。
 */
abstract class BaseInstaller(protected val ctx: Context) : IAppInstaller {

    protected val packageInstaller: PackageInstaller
        get() = ctx.packageManager.packageInstaller

    /** 创建安装 Session */
    protected fun createSession(): Int {
        val params = PackageInstaller.SessionParams(
            PackageInstaller.SessionParams.MODE_FULL_INSTALL
        )
        return packageInstaller.createSession(params)
    }

    /** 将 APK 文件流式写入 Session */
    protected fun writeApkToSession(session: PackageInstaller.Session, apkFile: File) {
        session.openWrite("package.apk", 0, apkFile.length()).use { output ->
            FileInputStream(apkFile).use { input ->
                input.copyTo(output)
            }
            session.fsync(output)
        }
    }

    /** commit 所需的 IntentSender（通过 InstallResultReceiver 投递） */
    protected fun commitIntent(sessionId: Int) =
        InstallResultReceiver.createPendingIntent(ctx, sessionId).intentSender

    /** 查询当前应用版本 */
    protected fun getCurrentVersion(): Long = try {
        ctx.packageManager
            .getPackageInfo(ctx.packageName, 0)
            .longVersionCode
    } catch (e: Exception) {
        0L
    }
}
