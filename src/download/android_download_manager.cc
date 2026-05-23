// ============================================================
// download/android_download_manager.cc - Android 下载管理器实现
// ============================================================
// 通过 JNI 调用 Kotlin 层的 onDownloadReady()，触发下载+安装流程。
//
// Kotlin 层已有完整实现（DeviceAgentService.kt）：
//   - HttpURLConnection + Range 断点续传
//   - SHA256 边下边算
//   - 进度上报 → POST /api/v1/devices/{id}/release_status
//   - pm install 安装
//   - 完成回报 → POST /api/v1/devices/{id}/report
//
// 本类是 JNI 桥接层，将 C++ 调用转发给 Kotlin，不做重复实现。
//
// 注意：Kotlin 层是异步执行的（开新线程），download() 立即返回。
// 进度和结果由 Kotlin 层直接通过 HTTP 上报给 serve。
// on_complete 回调在 JNI 调用成功后立即触发（表示"已转发"）。
// ============================================================

#ifdef __ANDROID__

#include "download/android_download_manager.h"
#include "jni_bridge.h"
#include "logger/logger.h"

#include <jni.h>

namespace device_agent {

// ─── JNI 工具：获取 JNIEnv ────────────────────────────────
// 从全局 JVM 获取当前线程的 JNIEnv。
// Android NDK 要求每个线程有自己的 JNIEnv。
static JNIEnv* getEnv() {
    if (g_jvm == nullptr) return nullptr;
    JNIEnv* env = nullptr;
    int status = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return nullptr;
        }
    } else if (status != JNI_OK) {
        return nullptr;
    }
    return env;
}

// ─── download ─────────────────────────────────────────────
// 调用 Kotlin onDownloadReady()，触发下载+安装流程。
// Kotlin 异步执行，本方法立即返回。
// 进度和结果由 Kotlin 层通过 HTTP 直接上报给 serve。
void AndroidDownloadManager::download(
        const DownloadRequest& req,
        ProgressCallback on_progress,
        CompleteCallback on_complete) {

    std::lock_guard<std::mutex> lock(mu_);
    // 不再阻止重复下载：Kotlin 层已有 inFlightFiles 去重，
    // C++ 层阻拦会导致 downloading_ 标志卡死，后续所有下发均被拦截。
    if (downloading_.load()) {
        LOG_INFO("AndroidDownloadManager: downloading flag was stuck, resetting");
        downloading_.store(false);
    }

    JNIEnv* env = getEnv();
    if (env == nullptr || g_java_service == nullptr) {
        std::string err = "Java service not available";
        LOG_ERROR("AndroidDownloadManager: " + err);
        if (on_complete) on_complete(false, err);
        return;
    }

    jclass serviceCls = env->GetObjectClass(g_java_service);
    jmethodID method = env->GetMethodID(
        serviceCls, "onDownloadReady",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (method == nullptr) {
        std::string err = "onDownloadReady method not found in Java service";
        LOG_ERROR("AndroidDownloadManager: " + err);
        if (on_complete) on_complete(false, err);
        return;
    }

    LOG_INFO("AndroidDownloadManager: downloadReady batch=" + req.batch_id +
             " file=" + req.file_id + " type=" + req.file_type +
             " cmd_id=" + req.command_id);

    jstring jBatch = env->NewStringUTF(req.batch_id.c_str());
    jstring jFile  = env->NewStringUTF(req.file_id.c_str());
    jstring jType  = env->NewStringUTF(req.file_type.c_str());
    jstring jUrl   = env->NewStringUTF(req.url.c_str());
    jstring jSha   = env->NewStringUTF(req.expected_sha256.c_str());
    jstring jCmdId = env->NewStringUTF(req.command_id.c_str());
    // HTTP path does not consume P2P fields; pass empty strings for JNI signature compatibility.
    jstring jTorrent = env->NewStringUTF("");
    jstring jMagnet = env->NewStringUTF("");

    env->CallVoidMethod(g_java_service, method,
        jBatch, jFile, jType, jUrl, jSha, static_cast<jlong>(req.file_size), jCmdId,
        jTorrent, jMagnet);

    env->DeleteLocalRef(jBatch);
    env->DeleteLocalRef(jFile);
    env->DeleteLocalRef(jType);
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jSha);
    env->DeleteLocalRef(jCmdId);
    env->DeleteLocalRef(jTorrent);
    env->DeleteLocalRef(jMagnet);

    // 检查 JNI 调用是否成功（是否有异常抛出）
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        std::string err = "JNI call to onDownloadReady failed";
        LOG_ERROR("AndroidDownloadManager: " + err);
        if (on_complete) on_complete(false, err);
        return;
    }

    downloading_.store(true);
    LOG_INFO("AndroidDownloadManager: download_ready forwarded to Java layer");

    // on_complete 回调表示"已转发"，实际下载结果由 Kotlin 层通过 HTTP 上报
    if (on_complete) on_complete(true, "");
}

// ─── cancel ───────────────────────────────────────────────
void AndroidDownloadManager::cancel() {
    // TODO: 通知 Kotlin 层取消下载（需要新增 JNI 方法）
    downloading_.store(false);
}

// ─── is_downloading ───────────────────────────────────────
bool AndroidDownloadManager::is_downloading() const {
    return downloading_.load();
}

}  // namespace device_agent

#endif  // __ANDROID__
