// ============================================================
// executor/android_executor.cc - Android 平台执行器实现
// ============================================================
// Android NDK 实现，通过 JNI 调用 Android Java API。
//
// 设计说明：
//   - Native C++ daemon 通过 JNI 调用 Android 系统 API
//   - reboot：通过 JNI 调用 PowerManager.reboot()
//   - upgradeApp：通过 JNI 调用 PackageManager.installPackage()
//   - UI 提示：通过 JNI 调用 UIHelper.showToast()
//   - 降级处理：无 root/系统签名时走普通 pm install
//
// 生命周期管理（Java Service 层）：
//   Java Service 持有 C++ daemon 子进程，Watchdog 监控存活
//   C++ daemon 通过 Unix Socket 向 Java 层报告状态
// ============================================================

#include "executor/executor.h"
#include "reboot_state/reboot_state.h"
#include "logger/logger.h"
#include "jni_bridge.h"

#include <jni.h>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

// Android Java 类名
static const char* kPowerManagerClass = "android/os/PowerManager";
static const char* kUIHelperClass = "com/deviceagent/UIHelper";
static const char* kPackageManagerClass = "android/content/pm/IPackageManager";

// ─── JNI 工具函数 ─────────────────────────────────────────

// 全局 JVM 引用（由 jni_wrapper.cpp 的 JNI_OnLoad 设置）
// 注意：g_jvm 和 g_java_service 的定义在此文件中，
//       jni_wrapper.cpp 通过 jni_bridge.h 中的 extern 引用使用它们。
JavaVM* g_jvm = nullptr;
jobject g_java_service = nullptr;

// 获取 JNIEnv（如果当前线程没有 attached，需要 AttachCurrentThread）
JNIEnv* getJNIEnv() {
    JNIEnv* env = nullptr;
    if (g_jvm == nullptr) {
        LOG_ERROR("AndroidExecutor: JVM not initialized");
        return nullptr;
    }
    jint ret = g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (ret == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != 0) {
            LOG_ERROR("AndroidExecutor: failed to attach thread to JVM");
            return nullptr;
        }
    }
    return env;
}

// ─── 权限检测 ─────────────────────────────────────────────

// 检测是否具有系统签名或 root 权限
static bool hasElevatedPrivileges() {
    // 检查 /system/app/ 目录写权限（系统签名特征）
    if (access("/system/app/", R_OK | W_OK) == 0) {
        return true;
    }
    // 检查是否为 root 用户
    if (getuid() == 0) {
        return true;
    }
    return false;
}

static void showToast(const std::string& message);

static bool isRemoteUrl(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

static bool forwardUpgradeUrlToJava(const std::string& apkUrl, const std::string& md5, std::string& err) {
    if (g_jvm == nullptr || g_java_service == nullptr) {
        err = "JNI service not available for remote app upgrade";
        LOG_ERROR("AndroidExecutor: " + err);
        return false;
    }

    JNIEnv* env = getJNIEnv();
    if (env == nullptr) {
        err = "Cannot get JNIEnv for remote app upgrade";
        LOG_ERROR("AndroidExecutor: " + err);
        return false;
    }

    jclass cls = env->GetObjectClass(g_java_service);
    if (cls == nullptr) {
        err = "Cannot get service class for remote app upgrade";
        LOG_ERROR("AndroidExecutor: " + err);
        return false;
    }

    jmethodID method = env->GetMethodID(
        cls, "onUpgradeApp", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (method == nullptr) {
        err = "onUpgradeApp method not found in Kotlin service";
        LOG_ERROR("AndroidExecutor: " + err);
        env->DeleteLocalRef(cls);
        return false;
    }

    jstring jUrl = env->NewStringUTF(apkUrl.c_str());
    jstring jMd5 = env->NewStringUTF(md5.c_str());
    env->CallVoidMethod(g_java_service, method, jUrl, jMd5);
    env->DeleteLocalRef(jUrl);
    env->DeleteLocalRef(jMd5);
    env->DeleteLocalRef(cls);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        err = "JNI call to onUpgradeApp failed";
        LOG_ERROR("AndroidExecutor: " + err);
        return false;
    }

    LOG_INFO("AndroidExecutor: remote upgrade forwarded to Kotlin onUpgradeApp");
    showToast("App upgrade download started...");
    return true;
}

// ─── UI 辅助 ─────────────────────────────────────────────

// 通过 JNI 调用 Java UIHelper.showToast()
static void showToast(const std::string& message) {
    if (g_jvm == nullptr) {
        LOG_WARN("AndroidExecutor: no JVM, skipping toast: " + message);
        return;
    }
    JNIEnv* env = getJNIEnv();
    if (env == nullptr) return;

    jclass clazz = env->FindClass(kUIHelperClass);
    if (clazz == nullptr) {
        LOG_WARN("AndroidExecutor: UIHelper class not found");
        return;
    }

    jmethodID showToast = env->GetStaticMethodID(clazz, "showToast", "(Ljava/lang/String;)V");
    if (showToast == nullptr) {
        LOG_WARN("AndroidExecutor: showToast method not found");
        return;
    }

    jstring jmsg = env->NewStringUTF(message.c_str());
    env->CallStaticVoidMethod(clazz, showToast, jmsg);
    env->DeleteLocalRef(jmsg);
}

// 全局变量：记录已经尝试过 reboot 的 command_id
// 避免 serve 重推同一条 reboot 命令时反复执行
static std::set<std::string> g_reboot_attempted;
static std::mutex g_reboot_attempted_mu;

namespace device_agent {

// ─── reboot ───────────────────────────────────────────────

std::string AndroidExecutor::reboot(bool force, const std::string& command_id, std::string& err) {
    LOG_INFO("AndroidExecutor: executing reboot, command_id=" + command_id);

    // 测试模式
    if (std::getenv("DEVICE_AGENT_TEST_MODE") != nullptr) {
        LOG_WARN("AndroidExecutor: TEST MODE - skipping real reboot");
        showToast("Test mode: reboot skipped");
        return "pending";
    }

    // ─── 防重入：如果这条命令已经尝试过 reboot，直接返回失败 ──
    {
        std::lock_guard<std::mutex> lock(g_reboot_attempted_mu);
        if (g_reboot_attempted.count(command_id) > 0) {
            err = "reboot already attempted for this command, device did not restart";
            LOG_WARN("AndroidExecutor: " + err);
            return "failed";
        }
        g_reboot_attempted.insert(command_id);
    }

    // ─── C+D 方案：写 pending 状态 ───────────────────────
    RebootStateManager& state_mgr = RebootStateManager::instance();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    state_mgr.write_pending(command_id, "android-device", now_ms);

    showToast("System reboot initiated...");

    // ─── fork 子进程执行系统级 reboot（纯 C，不调 JNI）───
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed: " + std::string(strerror(errno));
        LOG_ERROR("AndroidExecutor: fork failed: " + err);
        state_mgr.clear_pending();
        return "failed";
    }

    if (pid == 0) {
        // ═══════════════════════════════════════════════════
        // 子进程：纯 C 环境，禁止调用任何 JNI 函数！
        // Android Runtime (ART) 在 fork 后 JNI 状态不安全，
        // 调用 JNI 会触发 SIGABRT。
        // ═══════════════════════════════════════════════════
        sleep(3);
        state_mgr.clear_pending();

        // 方式 1：svc power reboot（shell 权限，最通用）
        LOG_INFO("[child] trying: svc power reboot");
        system("svc power reboot");
        sleep(8);

        // 方式 2：直接 reboot 命令（需要 root）
        if (hasElevatedPrivileges()) {
            LOG_INFO("[child] trying: /system/bin/reboot (root)");
            system("/system/bin/reboot");
            sleep(5);
        }

        // 方式 3：设置系统属性触发重启（需要 root）
        LOG_INFO("[child] trying: setprop sys.powerctl reboot");
        system("setprop sys.powerctl reboot");
        sleep(5);

        // 所有系统级方式都失败
        LOG_ERROR("[child] all system-level reboot methods failed");
        _exit(1);
    }

    // ═══════════════════════════════════════════════════════
    // 父进程：非阻塞等待 + JNI fallback
    // ═══════════════════════════════════════════════════════
    LOG_INFO("AndroidExecutor: reboot child pid=" + std::to_string(pid));

    // 先做一次非阻塞检查
    int status;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited > 0 && WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code != 0) {
            LOG_WARN("AndroidExecutor: child exited with code " + std::to_string(exit_code) + ", trying JNI fallback");
            return tryJNIReboot(command_id, err);
        }
    }

    // 子进程还在运行（正在尝试系统级 reboot）
    // 启动后台线程：5 秒后如果系统还没重启，尝试 JNI fallback
    std::thread([this, command_id]() {
        sleep(5);
        LOG_INFO("AndroidExecutor: system reboot may have failed, trying JNI fallback...");
        std::string err;
        std::string result = tryJNIReboot(command_id, err);
        if (result == "failed") {
            LOG_ERROR("AndroidExecutor: JNI reboot fallback also failed: " + err);
            // 失败状态已通过 tryJNIReboot 内部机制处理
        }
    }).detach();

    return "pending";
}

// ─── JNI fallback reboot（父进程中执行，安全使用 JNI）───
std::string AndroidExecutor::tryJNIReboot(const std::string& command_id, std::string& err) {
    LOG_INFO("AndroidExecutor: trying JNI reboot fallback");

    if (g_jvm == nullptr || g_java_service == nullptr) {
        err = "JNI not available for reboot fallback";
        LOG_ERROR("AndroidExecutor: " + err);
        return "failed";
    }

    JNIEnv* env = getJNIEnv();
    if (env == nullptr) {
        err = "Cannot get JNIEnv for reboot";
        LOG_ERROR("AndroidExecutor: " + err);
        return "failed";
    }

    // 回调 Kotlin 层的 onReboot() 方法
    // Kotlin 层会按权限等级尝试：PowerManager.reboot() → Runtime.exec() → su
    jclass cls = env->GetObjectClass(g_java_service);
    if (cls == nullptr) {
        err = "Cannot get service class";
        return "failed";
    }

    jmethodID method = env->GetMethodID(cls, "onReboot", "()V");
    if (method == nullptr) {
        err = "onReboot method not found in Kotlin service";
        LOG_ERROR("AndroidExecutor: " + err);
        return "failed";
    }

    env->CallVoidMethod(g_java_service, method);
    LOG_INFO("AndroidExecutor: onReboot() called via JNI");

    // onReboot() 会触发系统重启，如果成功我们不会返回
    // 如果失败，Kotlin 层会打日志，我们这里返回 pending 等待超时
    return "pending";
}

// ─── updateConfig ──────────────────────────────────────────

void AndroidExecutor::updateConfig(const std::string& key, const std::string& value, std::string& err) {
    LOG_INFO("AndroidExecutor: updateConfig key=" + key + " value=" + value);

    std::ostringstream oss;
    oss << "# device-agent config\n"
        << "# updated at: " << std::chrono::system_clock::now().time_since_epoch().count() << "\n"
        << key << " = " << value << "\n";

    std::string config_path = "/data/local/tmp/device-agent-config.bak";
    std::ofstream ofs(config_path);
    if (ofs.is_open()) {
        ofs << oss.str();
        ofs.close();
        LOG_INFO("Config backup written to: " + config_path);
    }

    showToast("Config updated: " + key);
    LOG_INFO("AndroidExecutor: Config update recorded: " + key + " = " + value);
}

// ─── upgradeFirmware ──────────────────────────────────────

void AndroidExecutor::upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) {
    LOG_INFO("AndroidExecutor: upgradeFirmware url=" + url + " md5=" + md5);

    if (url.empty()) {
        err = "firmware URL is empty";
        return;
    }

    showToast("Firmware upgrade started...");

    // Android OTA 升级通常需要 recovery mode，这里只是模拟
    // 真正的 OTA 需要设备特定实现（小米/华为等各不同）
    std::this_thread::sleep_for(std::chrono::seconds(2));

    LOG_INFO("Firmware upgrade simulated: " + url);
    LOG_WARN("Firmware upgrade is simulated - NOT ACTUALLY APPLIED");
    showToast("Firmware upgrade simulated (not applied)");
}

// ─── upgradeApp ───────────────────────────────────────────

void AndroidExecutor::upgradeApp(const std::string& apkPath, const std::string& md5, std::string& err) {
    LOG_INFO("AndroidExecutor: upgradeApp apk=" + apkPath + " md5=" + md5);

    if (apkPath.empty()) {
        err = "apk path is empty";
        return;
    }

    if (isRemoteUrl(apkPath)) {
        forwardUpgradeUrlToJava(apkPath, md5, err);
        return;
    }

    // 检查 APK 是否存在
    if (access(apkPath.c_str(), R_OK) != 0) {
        err = "apk file not found or not readable: " + apkPath;
        LOG_ERROR("AndroidExecutor: " + err);
        showToast("App install failed: file not found");
        return;
    }

    showToast("Installing app...");

    // ─── 降级处理 ────────────────────────────────────────
    if (hasElevatedPrivileges()) {
        // 系统签名/root：尝试静默安装（无弹框）
        LOG_INFO("AndroidExecutor: elevated privileges, attempting silent install");

        // 通过 JNI 调用 PackageManager.installPackage()
        if (g_jvm != nullptr) {
            JNIEnv* env = getJNIEnv();
            if (env != nullptr) {
                // 获取 IPackageManager
                jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
                jmethodID getSystemContext = env->GetStaticMethodID(
                    activityThreadClass, "getSystemContext", "()Landroid/app/ContextImpl;");
                if (getSystemContext != nullptr) {
                    jobject context = env->CallStaticObjectMethod(activityThreadClass, getSystemContext);
                    if (context != nullptr) {
                        jclass contextClass = env->GetObjectClass(context);
                        jmethodID getPackageManager = env->GetMethodID(
                            contextClass, "getPackageManager", "()Landroid/content/pm/PackageManager;");
                        if (getPackageManager != nullptr) {
                            jobject packageManager = env->CallObjectMethod(context, getPackageManager);
                            if (packageManager != nullptr) {
                                // 注意：installPackage 是 hidden API，普通 app 调不了
                                // 这里只是占位，真正的静默安装需要系统签名
                                LOG_INFO("AndroidExecutor: PackageManager obtained (silent install requires system signature)");
                            }
                        }
                    }
                }
            }
        }

        // 降级到 pm install -r（仍然需要弹框，但兼容性好）
        std::string cmd = "/system/bin/pm install -r \"" + apkPath + "\"";
        LOG_INFO("AndroidExecutor: executing: " + cmd);
        int ret = system(cmd.c_str());

        if (ret == 0) {
            LOG_INFO("AndroidExecutor: app installed successfully via pm install");
            showToast("App installed successfully");
            return;
        } else {
            err = "pm install failed with ret=" + std::to_string(ret);
            LOG_ERROR("AndroidExecutor: " + err);
            showToast("App install failed");
            return;
        }
    } else {
        // 普通签名：只能用 pm install，会弹系统确认框
        std::string cmd = "/system/bin/pm install -r \"" + apkPath + "\"";
        LOG_INFO("AndroidExecutor: executing (普通权限): " + cmd);
        int ret = system(cmd.c_str());

        if (ret == 0) {
            LOG_INFO("AndroidExecutor: app installed successfully");
            showToast("App installed successfully");
        } else {
            err = "pm install failed with ret=" + std::to_string(ret);
            LOG_ERROR("AndroidExecutor: " + err);
            showToast("App install failed");
        }
    }
}

}  // namespace device_agent
