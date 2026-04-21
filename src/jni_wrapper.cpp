// ============================================================
// jni_wrapper.cpp - JNI 包装层
// ============================================================
// ============================================================
// Kotlin companion object 方法（@JvmStatic）→ Java: DeviceAgentService.nativeStart()
// C++ JNI: (JNIEnv*, jclass, ...) = static method
//
// 调用顺序：
//   1. System.loadLibrary("device-agent") → JNI_OnLoad
//   2. onCreate() → nativeStart(serverHost, serverPort)  ← companion object @JvmStatic
//   3. onDestroy() → nativeStop()
// ============================================================

#include <jni.h>
#include <string>
#include <memory>
#include <mutex>
#include <android/log.h>
#include "client/device_client.h"
#include "client/command_handler.h"
#include "config/config.h"
#include "logger/logger.h"
#include "executor/executor.h"
#include "jni_bridge.h"

#define LOG_TAG "DeviceAgentJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── 前向声明 ─────────────────────────────────────────

// JNI native 方法实现
static jint Java_com_deviceagent_DeviceAgentService_nativeStart_impl(JNIEnv* env, jclass clazz, jobject serviceObj, jstring jServerHost, jint jServerPort);
static void Java_com_deviceagent_DeviceAgentService_nativeStop_impl(JNIEnv* env, jclass clazz);
static void Java_com_deviceagent_DeviceAgentService_onUpgradeApp_impl(JNIEnv* env, jclass clazz, jstring jApkUrl, jstring jMd5);
static void Java_com_deviceagent_DeviceAgentService_onDownloadReady_impl(JNIEnv* env, jclass clazz, jstring jBatchId, jstring jFileId, jstring jFileType, jstring jDownloadUrl, jstring jSha256, jlong jFileSize);
static void Java_com_deviceagent_DeviceAgentService_nativeReportReleaseStatus_impl(JNIEnv* env, jclass clazz, jstring jBatchId, jstring jFileId, jstring jStatus, jlong jDownloadedBytes, jstring jErrorCode, jstring jErrorMessage);
static void Java_com_deviceagent_DeviceAgentService_nativeUpgradeFirmware_impl(JNIEnv* env, jclass clazz, jstring jFilePath);
static jstring Java_com_deviceagent_DeviceAgentService_nativeDispatchCommand_impl(JNIEnv* env, jclass clazz, jstring jCmdType, jstring jPayloadJson);

// ─── 全局变量（跨函数共享）────────────────────────────
// g_jvm / g_java_service 在 jni_bridge.h 声明为 extern（由 android_executor.cc 定义）
// g_client / g_client_mutex 仅在此文件使用
static std::shared_ptr<device_agent::DeviceClient> g_client;
static std::mutex g_client_mutex;

// ─── JNI_OnLoad（库加载时保存 JVM + 注册所有 native 方法）──

// JNI 方法表（用于 RegisterNatives）
static JNINativeMethod g_methods[] = {
    {
        const_cast<char*>("nativeStart"),
        const_cast<char*>("(Ljava/lang/Object;Ljava/lang/String;I)I"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeStart_impl)
    },
    {
        const_cast<char*>("nativeStop"),
        const_cast<char*>("()V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeStop_impl)
    },
    {
        const_cast<char*>("onUpgradeApp"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_onUpgradeApp_impl)
    },
    {
        const_cast<char*>("onDownloadReady"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_onDownloadReady_impl)
    },
    {
        const_cast<char*>("nativeReportReleaseStatus"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JLjava/lang/String;Ljava/lang/String;)V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeReportReleaseStatus_impl)
    },
    {
        const_cast<char*>("nativeUpgradeFirmware"),
        const_cast<char*>("(Ljava/lang/String;)V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeUpgradeFirmware_impl)
    },
    {
        const_cast<char*>("nativeDispatchCommand"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeDispatchCommand_impl)
    }
};

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_jvm = vm;

    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        LOGE("JNI_OnLoad: GetEnv failed");
        return JNI_ERR;
    }

    // 查找 DeviceAgentService 类
    jclass cls = env->FindClass("com/deviceagent/DeviceAgentService");
    if (cls == nullptr) {
        LOGE("JNI_OnLoad: FindClass failed");
        return JNI_ERR;
    }

    // 硬编码注册，跳过 JNI 名称查找
    const jint method_count = sizeof(g_methods) / sizeof(g_methods[0]);
    if (env->RegisterNatives(cls, g_methods, method_count) != 0) {
        LOGE("JNI_OnLoad: RegisterNatives failed");
        return JNI_ERR;
    }

    LOGI("JNI_OnLoad: JVM initialized, registered %d native methods", method_count);
    return JNI_VERSION_1_6;
}

// ─── Toast 回调 ─────────────────────────────────────────

static void android_show_toast(const std::string& msg) {
    JNIEnv* env = getJNIEnv();
    if (env == nullptr || g_java_service == nullptr) {
        LOGI("Toast (no service): %s", msg.c_str());
        return;
    }
    jclass clazz = env->GetObjectClass(g_java_service);
    if (clazz == nullptr) {
        LOGE("Toast: service class not found");
        return;
    }
    jmethodID showToast = env->GetMethodID(clazz, "showToast", "(Ljava/lang/String;)V");
    if (showToast == nullptr) {
        LOGE("Toast: showToast method not found");
        return;
    }
    jstring jmsg = env->NewStringUTF(msg.c_str());
    env->CallVoidMethod(g_java_service, showToast, jmsg);
    env->DeleteLocalRef(jmsg);
}

// ─── 命令执行回调 ─────────────────────────────────────────

static bool report_command_resultJNI(const terminal_agent::v1::CommandResult& result) {
    JNIEnv* env = getJNIEnv();
    if (env == nullptr || g_java_service == nullptr) return false;
    jclass clazz = env->GetObjectClass(g_java_service);
    if (clazz == nullptr) return false;
    jmethodID method = env->GetMethodID(clazz, "onCommandResult", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (method == nullptr) return false;
    jstring jCmdId = env->NewStringUTF(result.command_id().c_str());
    jstring jStatus = env->NewStringUTF(result.status().c_str());
    env->CallVoidMethod(g_java_service, method, jCmdId, jStatus);
    env->DeleteLocalRef(jCmdId);
    env->DeleteLocalRef(jStatus);
    return true;
}

// ─── nativeStart 实现 ───────────────────────────────────
// Kotlin 签名: nativeStart(Any, String, Int): Int
// C++ 签名:   (JNIEnv*, jclass, jobject(service), jstring(host), jint(port)) -> jint
static jint Java_com_deviceagent_DeviceAgentService_nativeStart_impl(
    JNIEnv* env,
    jclass clazz,      // DeviceAgentService class (static method)
    jobject serviceObj, // passed Service instance (Any)
    jstring jServerHost,
    jint jServerPort) {

    (void)clazz;

    // 保存 Service 实例用于后续回调
    if (g_java_service != nullptr) {
        env->DeleteGlobalRef(g_java_service);
    }
    g_java_service = env->NewGlobalRef(serviceObj);
    LOGI("nativeStart: service registered");

    const char* serverHost = env->GetStringUTFChars(jServerHost, nullptr);
    std::string host = serverHost;
    env->ReleaseStringUTFChars(jServerHost, serverHost);

    device_agent::Config config;
    config = device_agent::Config::load_from_env();

    config.server.host = host;
    config.server.port = jServerPort;
    config.server.use_tls = false;

    if (config.auth.device_id.empty()) {
        config.auth.device_id = "ANDROID-001";
    }
    if (config.auth.token.empty()) {
        config.auth.token = "test-token-123";
    }

    LOGI("nativeStart: server=%s:%d, device=%s",
         config.server.host.c_str(), config.server.port,
         config.auth.device_id.c_str());

    auto executor = std::make_shared<device_agent::AndroidExecutor>();
    auto client = std::make_shared<device_agent::DeviceClient>(config);

    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        g_client = client;
    }

    device_agent::CommandHandler handler(
        [client](const terminal_agent::v1::CommandResult& result) -> bool {
            return client->report_command_result(result);
        });
    handler.set_executor(executor);

    client->set_command_callback(
        [&handler](const terminal_agent::v1::Command& cmd) {
            handler.handle(cmd);
        });

    client->start();
    LOGI("device-agent native client started");
    android_show_toast("device-agent connected");

    return 0;
}

// ─── nativeStop 实现 ───────────────────────────────────
static void Java_com_deviceagent_DeviceAgentService_nativeStop_impl(
    JNIEnv* env,
    jclass clazz) {

    (void)env;
    (void)clazz;
    LOGI("nativeStop called");
    if (g_java_service != nullptr) {
        JNIEnv* env_local = getJNIEnv();
        if (env_local != nullptr) {
            env_local->DeleteGlobalRef(g_java_service);
        }
        g_java_service = nullptr;
    }
    g_jvm = nullptr;
}

// ─── onUpgradeApp 实现 ─────────────────────────────────
// Kotlin: onUpgradeApp(apkUrl: String, md5: String)
// JNI:    (JNIEnv*, jclass, jstring(url), jstring(md5)) → void
static void Java_com_deviceagent_DeviceAgentService_onUpgradeApp_impl(
    JNIEnv* env,
    jclass clazz,
    jstring jApkUrl,
    jstring jMd5) {

    (void)clazz;

    if (g_java_service == nullptr) {
        LOGE("onUpgradeApp: g_java_service is null!");
        return;
    }

    const char* apkUrl = env->GetStringUTFChars(jApkUrl, nullptr);
    const char* md5 = env->GetStringUTFChars(jMd5, nullptr);

    LOGI("onUpgradeApp: url=%s, md5=%s", apkUrl, md5);

    // 转换为 jstring 并调用 Java 方法
    jstring jUrl = env->NewStringUTF(apkUrl);
    jstring jMd5Str = env->NewStringUTF(md5);

    jclass serviceCls = env->GetObjectClass(g_java_service);
    jmethodID method = env->GetMethodID(serviceCls, "onUpgradeApp", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (method != nullptr) {
        env->CallVoidMethod(g_java_service, method, jUrl, jMd5Str);
        LOGI("onUpgradeApp: Java method called successfully");
    } else {
        LOGE("onUpgradeApp: method not found");
    }

    env->ReleaseStringUTFChars(jApkUrl, apkUrl);
    env->ReleaseStringUTFChars(jMd5, md5);
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jMd5Str);
}

// ─── onDownloadReady 实现 ────────────────────────────────
// Kotlin: onDownloadReady(batchId, fileId, fileType, downloadUrl, sha256, fileSize)
// JNI:    (JNIEnv*, jclass, jstring x6) → void
static void Java_com_deviceagent_DeviceAgentService_onDownloadReady_impl(
    JNIEnv* env,
    jclass clazz,
    jstring jBatchId,
    jstring jFileId,
    jstring jFileType,
    jstring jDownloadUrl,
    jstring jSha256,
    jlong jFileSize) {

    (void)clazz;

    const char* batchId    = env->GetStringUTFChars(jBatchId, nullptr);
    const char* fileId    = env->GetStringUTFChars(jFileId, nullptr);
    const char* fileType  = env->GetStringUTFChars(jFileType, nullptr);
    const char* url       = env->GetStringUTFChars(jDownloadUrl, nullptr);
    const char* sha256    = env->GetStringUTFChars(jSha256, nullptr);
    // jFileSize is jlong (no need to ReleaseStringUTFChars)

    LOGI("onDownloadReady: batch=%s file=%s type=%s url=%s sha=%s size=%lld",
         batchId, fileId, fileType, url, sha256, (long long)jFileSize);

    // 将下载任务转发给 Java 层（onDownloadReady Kotlin 扩展函数）
    jstring jBatch   = env->NewStringUTF(batchId);
    jstring jFile    = env->NewStringUTF(fileId);
    jstring jType    = env->NewStringUTF(fileType);
    jstring jUrl     = env->NewStringUTF(url);
    jstring jSha     = env->NewStringUTF(sha256);

    jclass serviceCls = env->GetObjectClass(g_java_service);
    jmethodID method = env->GetMethodID(
        serviceCls, "onDownloadReady",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;J)V");
    if (method != nullptr) {
        env->CallVoidMethod(g_java_service, method,
            jBatch, jFile, jType, jUrl, jSha, jFileSize);
        LOGI("onDownloadReady: Java method called");
    } else {
        LOGE("onDownloadReady: method not found");
    }

    env->ReleaseStringUTFChars(jBatchId, batchId);
    env->ReleaseStringUTFChars(jFileId, fileId);
    env->ReleaseStringUTFChars(jFileType, fileType);
    env->ReleaseStringUTFChars(jDownloadUrl, url);
    env->ReleaseStringUTFChars(jSha256, sha256);
    env->DeleteLocalRef(jBatch);
    env->DeleteLocalRef(jFile);
    env->DeleteLocalRef(jType);
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jSha);
}

// ─── nativeReportReleaseStatus 实现 ─────────────────────
// Kotlin 调 native，上报下载/安装状态给 native 层
// native 层通过 gRPC ReportReleaseStatus 发送到服务端
static void Java_com_deviceagent_DeviceAgentService_nativeReportReleaseStatus_impl(
    JNIEnv* env,
    jclass clazz,
    jstring jBatchId,
    jstring jFileId,
    jstring jStatus,
    jlong jDownloadedBytes,
    jstring jErrorCode,
    jstring jErrorMessage) {

    (void)clazz;

    const char* batchId  = env->GetStringUTFChars(jBatchId, nullptr);
    const char* fileId  = env->GetStringUTFChars(jFileId, nullptr);
    const char* status  = env->GetStringUTFChars(jStatus, nullptr);
    const char* errCode = env->GetStringUTFChars(jErrorCode, nullptr);
    const char* errMsg  = env->GetStringUTFChars(jErrorMessage, nullptr);

    LOGI("nativeReportReleaseStatus: batch=%s file=%s status=%s bytes=%lld err=%s:%s",
         batchId, fileId, status, (long long)jDownloadedBytes,
         errCode && errCode[0] ? errCode : "none",
         errMsg  && errMsg[0]  ? errMsg  : "none");

    // TODO: 通过 gRPC ReportReleaseStatus 上报到服务端
    // 当前 gen-android proto 尚未包含 ReleaseStatusRequest，
    // 等 proto 重新生成（protoc 4.25.1）后补全此处 gRPC 调用

    env->ReleaseStringUTFChars(jBatchId, batchId);
    env->ReleaseStringUTFChars(jFileId, fileId);
    env->ReleaseStringUTFChars(jStatus, status);
    env->ReleaseStringUTFChars(jErrorCode, errCode);
    env->ReleaseStringUTFChars(jErrorMessage, errMsg);
}

// ─── nativeUpgradeFirmware 实现 ──────────────────────────
// 将 system 固件升级请求转发给 AndroidExecutor
static void Java_com_deviceagent_DeviceAgentService_nativeUpgradeFirmware_impl(
    JNIEnv* env,
    jclass clazz,
    jstring jFilePath) {

    (void)clazz;

    const char* filePath = env->GetStringUTFChars(jFilePath, nullptr);
    LOGI("nativeUpgradeFirmware: path=%s", filePath);

    // 复用 AndroidExecutor 的 upgradeFirmware
    // 注意：这里在 JNI wrapper 线程中直接调用，不走 CommandHandler
    std::string err;
    device_agent::AndroidExecutor executor;
    executor.upgradeFirmware(filePath, "", err);

    env->ReleaseStringUTFChars(jFilePath, filePath);
}

// ─── nativeDispatchCommand 实现（方案 A 入口）────────────
// Kotlin 签名: external fun nativeDispatchCommand(type: String, payload: String): String
// 返回 JSON: {"status":"pending"|"completed"|"failed", "message":"..."}
static jstring Java_com_deviceagent_DeviceAgentService_nativeDispatchCommand_impl(
    JNIEnv* env, jclass clazz, jstring jCmdType, jstring jPayloadJson) {
    (void)clazz;

    const char* cmdType = env->GetStringUTFChars(jCmdType, nullptr);
    const char* payload = env->GetStringUTFChars(jPayloadJson, nullptr);
    std::string type = cmdType ? cmdType : "";
    std::string payloadStr = payload ? payload : "";
    env->ReleaseStringUTFChars(jCmdType, cmdType);
    env->ReleaseStringUTFChars(jPayloadJson, payload);

    LOGI("nativeDispatchCommand: type=%s payload=%s", type.c_str(), payloadStr.c_str());

    device_agent::AndroidExecutor executor;
    std::string status = "failed";
    std::string message;

    auto extract = [&](const std::string& key) -> std::string {
        std::string needle = "\"" + key + "\"";
        auto k = payloadStr.find(needle);
        if (k == std::string::npos) return "";
        auto colon = payloadStr.find(':', k);
        if (colon == std::string::npos) return "";
        auto q1 = payloadStr.find('"', colon + 1);
        if (q1 == std::string::npos) return "";
        auto q2 = payloadStr.find('"', q1 + 1);
        if (q2 == std::string::npos) return "";
        return payloadStr.substr(q1 + 1, q2 - q1 - 1);
    };

    std::string err;
    if (type == "reboot" || type == "REBOOT") {
        std::string force_str = extract("force");
        bool force = (force_str == "true");
        std::string cmdId = extract("command_id");
        status = executor.reboot(force, cmdId, err);
        message = err;
    } else if (type == "update_config" || type == "UPDATE_CONFIG") {
        std::string key = extract("key");
        std::string value = extract("value");
        if (key.empty()) {
            status = "failed";
            message = "empty key";
        } else {
            executor.updateConfig(key, value, err);
            status = err.empty() ? "completed" : "failed";
            message = err.empty() ? ("Updated " + key) : err;
        }
    } else {
        status = "failed";
        message = "unknown command type: " + type;
    }

    std::string result = "{\"status\":\"" + status + "\",\"message\":\"" + message + "\"}";
    return env->NewStringUTF(result.c_str());
}
