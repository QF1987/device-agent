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
#include <string_view>
#include <memory>
#include <mutex>
#include <android/log.h>
#include "client/device_client.h"
#include "client/command_handler.h"
#include "client/network_info.h"
#include "config/config.h"
#include "config/p2p_config_store.h"
#include "logger/logger.h"
#include "executor/executor.h"
#include "download/idownload_manager.h"
#include "download/android_download_manager.h"
#include "download/network_policy.h"
#include "download/p2p_download_manager.h"
#include "jni_bridge.h"

#define LOG_TAG "DeviceAgentJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── 前向声明 ─────────────────────────────────────────

// JNI native 方法实现
static jint Java_com_deviceagent_DeviceAgentService_nativeStart_impl(JNIEnv* env, jclass clazz, jobject serviceObj, jstring jServerHost, jint jServerPort, jstring jDeviceId);
static void Java_com_deviceagent_DeviceAgentService_nativeStop_impl(JNIEnv* env, jclass clazz);
static jboolean Java_com_deviceagent_DeviceAgentService_nativeReportReleaseStatus_impl(
    JNIEnv* env,
    jclass clazz,
    jstring jBatchId,
    jstring jFileId,
    jstring jStatus,
    jlong jDownloadedBytes,
    jstring jErrorCode,
    jstring jErrorMessage,
    jint jCompletionPath,
    jlong jPeerBytes,
    jlong jWebSeedBytes);
static void Java_com_deviceagent_DeviceAgentService_nativeOnNetworkChanged_impl(JNIEnv* env, jclass clazz, jboolean isCellular, jboolean isWifi);

// ─── 全局变量（跨函数共享）────────────────────────────
// g_jvm / g_java_service 在 jni_bridge.h 声明为 extern（由 android_executor.cc 定义）
// g_client / g_client_mutex 仅在此文件使用
static std::shared_ptr<device_agent::DeviceClient> g_client;
static std::shared_ptr<device_agent::Executor> g_executor;
static std::shared_ptr<device_agent::NetworkPolicy> g_network_policy;
static std::shared_ptr<device_agent::P2PConfigStore> g_p2p_config_store;
static std::unique_ptr<device_agent::CommandHandler> g_handler;
static std::mutex g_client_mutex;

static terminal_agent::v1::ReleaseDeviceStatus parseReleaseDeviceStatus(std::string_view status) {
    if (status == "pending") return terminal_agent::v1::RELEASE_DEVICE_STATUS_PENDING;
    if (status == "ready") return terminal_agent::v1::RELEASE_DEVICE_STATUS_READY;
    if (status == "downloading") return terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADING;
    if (status == "downloaded") return terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOADED;
    if (status == "installing") return terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLING;
    if (status == "installed") return terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALLED;
    if (status == "download_failed") return terminal_agent::v1::RELEASE_DEVICE_STATUS_DOWNLOAD_FAILED;
    if (status == "install_failed") return terminal_agent::v1::RELEASE_DEVICE_STATUS_INSTALL_FAILED;
    if (status == "cancelled") return terminal_agent::v1::RELEASE_DEVICE_STATUS_CANCELLED;
    if (status == "retrying") return terminal_agent::v1::RELEASE_DEVICE_STATUS_RETRYING;
    return terminal_agent::v1::RELEASE_DEVICE_STATUS_UNSPECIFIED;
}

static terminal_agent::v1::ReleaseErrorCode parseReleaseErrorCode(std::string_view code) {
    if (code == "NETWORK_ERROR") return terminal_agent::v1::RELEASE_ERROR_CODE_NETWORK_ERROR;
    if (code == "SERVER_ERROR") return terminal_agent::v1::RELEASE_ERROR_CODE_SERVER_ERROR;
    if (code == "STORAGE_ERROR") return terminal_agent::v1::RELEASE_ERROR_CODE_STORAGE_ERROR;
    if (code == "CHECKSUM_FAILED") return terminal_agent::v1::RELEASE_ERROR_CODE_CHECKSUM_FAILED;
    if (code == "INSTALL_ERROR") return terminal_agent::v1::RELEASE_ERROR_CODE_INSTALL_ERROR;
    if (code == "BUSINESS_ERROR") return terminal_agent::v1::RELEASE_ERROR_CODE_BUSINESS_ERROR;
    return terminal_agent::v1::RELEASE_ERROR_CODE_UNSPECIFIED;
}

static terminal_agent::v1::CompletionPath parseCompletionPath(jint path) {
    switch (path) {
        case 1:
            return terminal_agent::v1::P2P_PRIMARY;
        case 2:
            return terminal_agent::v1::WEB_SEED_PRIMARY;
        case 3:
            return terminal_agent::v1::HTTP_FALLBACK_STALL;
        case 4:
            return terminal_agent::v1::HTTP_FALLBACK_SHA_MISMATCH;
        default:
            return terminal_agent::v1::COMPLETION_PATH_UNSPECIFIED;
    }
}

// ─── JNI_OnLoad（库加载时保存 JVM + 注册所有 native 方法）──

// JNI 方法表（用于 RegisterNatives）
// 注意：只有 Kotlin "external fun" 声明的方法才能注册到 RegisterNatives。
// onUpgradeApp / onDownloadReady 等是普通 Kotlin 方法，C++ 通过 CallVoidMethod 回调，
// 不需要也不应该放在这里。
static JNINativeMethod g_methods[] = {
    {
        const_cast<char*>("nativeStart"),
        const_cast<char*>("(Ljava/lang/Object;Ljava/lang/String;ILjava/lang/String;)I"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeStart_impl)
    },
    {
        const_cast<char*>("nativeStop"),
        const_cast<char*>("()V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeStop_impl)
    },
    {
        const_cast<char*>("nativeReportReleaseStatus"),
        const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;JLjava/lang/String;Ljava/lang/String;IJJ)Z"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeReportReleaseStatus_impl)
    },
    {
        const_cast<char*>("nativeOnNetworkChanged"),
        const_cast<char*>("(ZZ)V"),
        reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeOnNetworkChanged_impl)
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

class ScopedJniEnv {
public:
    ScopedJniEnv() = default;
    ~ScopedJniEnv() {
        if (attached_ && g_jvm != nullptr) {
            g_jvm->DetachCurrentThread();
        }
    }

    ScopedJniEnv(const ScopedJniEnv&) = delete;
    ScopedJniEnv& operator=(const ScopedJniEnv&) = delete;
    ScopedJniEnv(ScopedJniEnv&& other) noexcept
        : env_(other.env_), attached_(other.attached_) {
        other.env_ = nullptr;
        other.attached_ = false;
    }
    ScopedJniEnv& operator=(ScopedJniEnv&&) = delete;

    JNIEnv* get() const { return env_; }

    static ScopedJniEnv current() {
        ScopedJniEnv scoped;
        if (g_jvm == nullptr) {
            return scoped;
        }
        jint status = g_jvm->GetEnv(reinterpret_cast<void**>(&scoped.env_), JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (g_jvm->AttachCurrentThread(&scoped.env_, nullptr) == JNI_OK) {
                scoped.attached_ = true;
            } else {
                scoped.env_ = nullptr;
            }
        } else if (status != JNI_OK) {
            scoped.env_ = nullptr;
        }
        return scoped;
    }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

static jclass get_service_class(JNIEnv* env) {
    if (env == nullptr || g_java_service == nullptr) {
        return nullptr;
    }
    return env->GetObjectClass(g_java_service);
}

static void clear_jni_exception(JNIEnv* env, const char* label) {
    if (env != nullptr && env->ExceptionCheck()) {
        LOGE("%s: Java exception", label);
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

static std::string jstring_to_std(JNIEnv* env, jstring value) {
    if (env == nullptr || value == nullptr) {
        return "";
    }
    const char* raw = env->GetStringUTFChars(value, nullptr);
    if (raw == nullptr) {
        return "";
    }
    std::string out(raw);
    env->ReleaseStringUTFChars(value, raw);
    return out;
}

static terminal_agent::v1::NetworkType proto_network_type_from_string(const std::string& value) {
    if (value == "WIFI") return terminal_agent::v1::WIFI;
    if (value == "CELLULAR") return terminal_agent::v1::CELLULAR;
    if (value == "ETHERNET") return terminal_agent::v1::ETHERNET;
    return terminal_agent::v1::NET_UNKNOWN;
}

static device_agent::NetworkInfoSnapshot collect_android_network_info() {
    device_agent::NetworkInfoSnapshot info;
    JNIEnv* env = getJNIEnv();
    jclass clazz = get_service_class(env);
    if (clazz == nullptr) {
        return info;
    }
    jmethodID method = env->GetMethodID(clazz, "collectNetworkInfoSnapshot", "()[Ljava/lang/String;");
    if (method == nullptr) {
        clear_jni_exception(env, "collectNetworkInfoSnapshot.GetMethodID");
        env->DeleteLocalRef(clazz);
        return info;
    }
    auto array = static_cast<jobjectArray>(env->CallObjectMethod(g_java_service, method));
    if (env->ExceptionCheck() || array == nullptr) {
        clear_jni_exception(env, "collectNetworkInfoSnapshot.CallObjectMethod");
        env->DeleteLocalRef(clazz);
        return info;
    }
    auto get = [&](jsize index) -> std::string {
        jstring item = static_cast<jstring>(env->GetObjectArrayElement(array, index));
        std::string value = jstring_to_std(env, item);
        if (item != nullptr) env->DeleteLocalRef(item);
        return value;
    };
    if (env->GetArrayLength(array) >= 8) {
        info.gateway_mac = get(0);
        info.bssid = get(1);
        info.lan_ip = get(2);
        info.lan_cidr = get(3);
        info.public_ip = get(4);
        info.net_type = proto_network_type_from_string(get(5));
        info.is_metered = get(6) == "true";
        info.is_roaming = get(7) == "true";
    }
    env->DeleteLocalRef(array);
    env->DeleteLocalRef(clazz);
    return info;
}

static void call_p2p_started(const device_agent::DownloadRequest& req, const std::string& localPath) {
    ScopedJniEnv scoped = ScopedJniEnv::current();
    JNIEnv* env = scoped.get();
    jclass clazz = get_service_class(env);
    if (clazz == nullptr) {
        LOGE("P2P callback: service unavailable for onP2PStarted");
        return;
    }

    jmethodID method = env->GetMethodID(
        clazz,
        "onP2PStarted",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (method == nullptr) {
        clear_jni_exception(env, "onP2PStarted");
        env->DeleteLocalRef(clazz);
        return;
    }

    jstring jBatch = env->NewStringUTF(req.batch_id.c_str());
    jstring jFile = env->NewStringUTF(req.file_id.c_str());
    jstring jCmd = env->NewStringUTF(req.command_id.c_str());
    jstring jSha = env->NewStringUTF(req.expected_sha256.c_str());
    jstring jPath = env->NewStringUTF(localPath.c_str());
    jstring jType = env->NewStringUTF(req.file_type.c_str());
    jstring jUrl = env->NewStringUTF(req.url.c_str());
    env->CallVoidMethod(g_java_service, method, jBatch, jFile, jCmd, jSha, jPath, jType, jUrl);
    clear_jni_exception(env, "onP2PStarted");
    env->DeleteLocalRef(jBatch);
    env->DeleteLocalRef(jFile);
    env->DeleteLocalRef(jCmd);
    env->DeleteLocalRef(jSha);
    env->DeleteLocalRef(jPath);
    env->DeleteLocalRef(jType);
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(clazz);
}

static void call_p2p_progress(const device_agent::DownloadProgress& progress) {
    ScopedJniEnv scoped = ScopedJniEnv::current();
    JNIEnv* env = scoped.get();
    jclass clazz = get_service_class(env);
    if (clazz == nullptr) {
        LOGE("P2P callback: service unavailable for onP2PProgress");
        return;
    }

    jmethodID method = env->GetMethodID(clazz, "onP2PProgress", "(IJJ)V");
    if (method == nullptr) {
        clear_jni_exception(env, "onP2PProgress");
        env->DeleteLocalRef(clazz);
        return;
    }

    env->CallVoidMethod(
        g_java_service,
        method,
        static_cast<jint>(progress.percent),
        static_cast<jlong>(progress.downloaded_bytes),
        static_cast<jlong>(progress.total_bytes));
    clear_jni_exception(env, "onP2PProgress");
    env->DeleteLocalRef(clazz);
}

static void call_p2p_complete(bool success,
                              const std::string& error,
                              device_agent::CompletionPathTelemetry completion_path,
                              int64_t peer_bytes,
                              int64_t web_seed_bytes) {
    ScopedJniEnv scoped = ScopedJniEnv::current();
    JNIEnv* env = scoped.get();
    jclass clazz = get_service_class(env);
    if (clazz == nullptr) {
        LOGE("P2P callback: service unavailable for onP2PComplete");
        return;
    }

    jmethodID method = env->GetMethodID(clazz, "onP2PComplete", "(ZLjava/lang/String;IJJ)V");
    if (method == nullptr) {
        clear_jni_exception(env, "onP2PComplete");
        env->DeleteLocalRef(clazz);
        return;
    }

    jstring jError = env->NewStringUTF(error.c_str());
    env->CallVoidMethod(
        g_java_service,
        method,
        success ? JNI_TRUE : JNI_FALSE,
        jError,
        static_cast<jint>(completion_path),
        static_cast<jlong>(peer_bytes),
        static_cast<jlong>(web_seed_bytes));
    clear_jni_exception(env, "onP2PComplete");
    env->DeleteLocalRef(jError);
    env->DeleteLocalRef(clazz);
}

// ─── nativeStart 实现 ───────────────────────────────────
// Kotlin 签名: nativeStart(Any, String, Int, String): Int
// C++ 签名:   (JNIEnv*, jclass, jobject(service), jstring(host), jint(port), jstring(deviceId)) -> jint
static jint Java_com_deviceagent_DeviceAgentService_nativeStart_impl(
    JNIEnv* env,
    jclass clazz,      // DeviceAgentService class (static method)
    jobject serviceObj, // passed Service instance (Any)
    jstring jServerHost,
    jint jServerPort,
    jstring jDeviceId) {

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

    std::string deviceId;
    if (jDeviceId != nullptr) {
        const char* rawDeviceId = env->GetStringUTFChars(jDeviceId, nullptr);
        deviceId = rawDeviceId;
        env->ReleaseStringUTFChars(jDeviceId, rawDeviceId);
    }

    device_agent::Config config;
    config = device_agent::Config::load_from_env();

    config.server.host = host;
    config.server.port = jServerPort;
    config.server.use_tls = false;

    if (!deviceId.empty()) {
        config.auth.device_id = deviceId;
    } else if (config.auth.device_id.empty()) {
        config.auth.device_id = "ANDROID-001";
    }
    if (config.auth.token.empty()) {
        config.auth.token = "test-token-123";
    }
    device_agent::set_network_info_provider(collect_android_network_info);

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
    g_network_policy = std::make_shared<device_agent::NetworkPolicy>();
    g_p2p_config_store = std::make_shared<device_agent::P2PConfigStore>();
    device_agent::P2PConfigStore::set_global(g_p2p_config_store);
    g_handler->set_p2p_config_store(g_p2p_config_store);
    device_agent::P2PDownloadManager::Callbacks p2pCallbacks;
    p2pCallbacks.on_started = [](const device_agent::DownloadRequest& req, const std::string& localPath) {
        call_p2p_started(req, localPath);
    };
    p2pCallbacks.on_progress = [](const device_agent::DownloadRequest&, const device_agent::DownloadProgress& progress) {
        call_p2p_progress(progress);
    };
    p2pCallbacks.on_complete_with_path = [](const device_agent::DownloadRequest&,
                                            const std::string&,
                                            bool success,
                                            const std::string& error,
                                            device_agent::CompletionPathTelemetry completion_path,
                                            int64_t peer_bytes,
                                            int64_t web_seed_bytes) {
        call_p2p_complete(success, error, completion_path, peer_bytes, web_seed_bytes);
    };
    g_handler->set_download_manager(std::make_shared<device_agent::P2PDownloadManager>(
        p2pCallbacks,
        device_agent::P2PSeedingPolicy::alpha_defaults(),
        g_network_policy));
    LOGI("nativeStart: handler configured with executor + p2p_download_manager");

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
        device_agent::P2PConfigStore::set_global(nullptr);
        g_p2p_config_store.reset();
        g_network_policy.reset();
        g_executor.reset();
    }
    device_agent::set_network_info_provider(nullptr);
    if (g_java_service != nullptr) {
        JNIEnv* env_local = getJNIEnv();
        if (env_local != nullptr) {
            env_local->DeleteGlobalRef(g_java_service);
        }
        g_java_service = nullptr;
    }
    g_jvm = nullptr;
}

// ─── nativeOnNetworkChanged 实现 ────────────────────────
// Kotlin签名: nativeOnNetworkChanged(isCellular, isWifi): Unit
static void Java_com_deviceagent_DeviceAgentService_nativeOnNetworkChanged_impl(
    JNIEnv* env,
    jclass clazz,
    jboolean isCellular,
    jboolean isWifi) {

    (void)env;
    (void)clazz;

    device_agent::NetworkType type = device_agent::NetworkType::NONE;
    if (isWifi == JNI_TRUE) {
        type = device_agent::NetworkType::WIFI;
    } else if (isCellular == JNI_TRUE) {
        type = device_agent::NetworkType::CELLULAR;
    } else {
        type = device_agent::NetworkType::NONE;
    }

    std::shared_ptr<device_agent::NetworkPolicy> policy;
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        policy = g_network_policy;
    }
    if (policy) {
        policy->on_network_changed(type);
    }
}

// ─── nativeReportReleaseStatus 实现 ──────────────────────
// Kotlin签名: nativeReportReleaseStatus(batchId, fileId, status, downloadedBytes, errorCode, errorMessage, completionPath, peerBytes, webSeedBytes): Boolean
static jboolean Java_com_deviceagent_DeviceAgentService_nativeReportReleaseStatus_impl(
    JNIEnv* env,
    jclass,
    jstring jBatchId,
    jstring jFileId,
    jstring jStatus,
    jlong jDownloadedBytes,
    jstring jErrorCode,
    jstring jErrorMessage,
    jint jCompletionPath,
    jlong jPeerBytes,
    jlong jWebSeedBytes) {

    if (g_client == nullptr) {
        LOGI("nativeReportReleaseStatus: g_client is null, skipping");
        return JNI_FALSE;
    }

    terminal_agent::v1::ReleaseStatusRequest req;

    const char* str = env->GetStringUTFChars(jBatchId, nullptr);
    req.set_batch_id(str);
    env->ReleaseStringUTFChars(jBatchId, str);

    str = env->GetStringUTFChars(jFileId, nullptr);
    req.set_file_id(str);
    env->ReleaseStringUTFChars(jFileId, str);

    str = env->GetStringUTFChars(jStatus, nullptr);
    req.set_status(parseReleaseDeviceStatus(str));
    env->ReleaseStringUTFChars(jStatus, str);

    req.set_downloaded_bytes(jDownloadedBytes);
    req.set_completion_path(parseCompletionPath(jCompletionPath));
    req.set_peer_bytes(jPeerBytes);
    req.set_web_seed_bytes(jWebSeedBytes);

    if (jErrorCode != nullptr) {
        str = env->GetStringUTFChars(jErrorCode, nullptr);
        req.set_error_code(parseReleaseErrorCode(str));
        env->ReleaseStringUTFChars(jErrorCode, str);
    }
    if (jErrorMessage != nullptr) {
        str = env->GetStringUTFChars(jErrorMessage, nullptr);
        req.set_error_message(str);
        env->ReleaseStringUTFChars(jErrorMessage, str);
    }

    req.set_timestamp(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );

    bool ok = g_client->report_release_status(req);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// ─── onUpgradeApp 实现 ─────────────────────────────────
// Kotlin: onUpgradeApp(apkUrl: String, md5: String, commandId: String)
// JNI:    (JNIEnv*, jclass, jstring(url), jstring(md5), jstring(commandId)) → void
// ─── onDownloadReady 实现 ────────────────────────────────
// Kotlin: onDownloadReady(batchId, fileId, fileType, downloadUrl, sha256, fileSize, commandId)
// JNI:    (JNIEnv*, jclass, jstring x7, jlong(fileSize)) → void
// JNI:    (JNIEnv*, jclass, jstring x6) → void
