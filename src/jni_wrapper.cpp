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
#include "download/idownload_manager.h"
#include "download/android_download_manager.h"
#include "jni_bridge.h"

#define LOG_TAG "DeviceAgentJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── 前向声明 ─────────────────────────────────────────

// JNI native 方法实现
static jint Java_com_deviceagent_DeviceAgentService_nativeStart_impl(JNIEnv* env, jclass clazz, jobject serviceObj, jstring jServerHost, jint jServerPort);
static void Java_com_deviceagent_DeviceAgentService_nativeStop_impl(JNIEnv* env, jclass clazz);

// ─── 全局变量（跨函数共享）────────────────────────────
// g_jvm / g_java_service 在 jni_bridge.h 声明为 extern（由 android_executor.cc 定义）
// g_client / g_client_mutex 仅在此文件使用
static std::shared_ptr<device_agent::DeviceClient> g_client;
static std::shared_ptr<device_agent::Executor> g_executor;
static std::unique_ptr<device_agent::CommandHandler> g_handler;
static std::mutex g_client_mutex;

// ─── JNI_OnLoad（库加载时保存 JVM + 注册所有 native 方法）──

// JNI 方法表（用于 RegisterNatives）
// 注意：只有 Kotlin "external fun" 声明的方法才能注册到 RegisterNatives。
// onUpgradeApp / onDownloadReady 等是普通 Kotlin 方法，C++ 通过 CallVoidMethod 回调，
// 不需要也不应该放在这里。
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
        g_executor = executor;
    }

    // 存为全局变量，避免 JNI 函数返回后 handler 被析构
    // （command_callback_ 捕获了 handler 引用，handler 必须比 client 活得久）
    g_handler = std::make_unique<device_agent::CommandHandler>(
        [client](const terminal_agent::v1::CommandResult& result) -> bool {
            return client->report_command_result(result);
        });
    g_handler->set_executor(executor);
    g_handler->set_download_manager(std::make_shared<device_agent::AndroidDownloadManager>());
    LOGI("nativeStart: handler configured with executor + download_manager");

    client->set_command_callback(
        [](const terminal_agent::v1::Command& cmd) {
            if (g_handler) {
                g_handler->handle(cmd);
            }
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
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        if (g_client) {
            g_client->stop();
            g_client.reset();
        }
        g_handler.reset();
        g_executor.reset();
    }
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
// ─── onDownloadReady 实现 ────────────────────────────────
// Kotlin: onDownloadReady(batchId, fileId, fileType, downloadUrl, sha256, fileSize)
// JNI:    (JNIEnv*, jclass, jstring x6) → void
