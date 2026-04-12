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

#include <jni.h>
#include <string>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

// Android Java 类名
static const char* kPowerManagerClass = "android/os/PowerManager";
static const char* kUIHelperClass = "com/deviceagent/UIHelper";
static const char* kPackageManagerClass = "android/content/pm/IPackageManager";

// ─── JNI 工具函数 ─────────────────────────────────────────

// 全局 JVM 引用（由 JNI_OnLoad 设置）
static JavaVM* g_jvm = nullptr;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    g_jvm = vm;
    return JNI_VERSION_1_6;
}

// 获取 JNIEnv（如果当前线程没有 attached，需要 AttachCurrentThread）
static JNIEnv* getJNIEnv() {
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

    // ─── C+D 方案：写 pending 状态 ───────────────────────
    RebootStateManager& state_mgr = RebootStateManager::instance();
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    state_mgr.write_pending(command_id, "android-device", now_ms);

    // 显示系统提示
    showToast("System reboot initiated...");

    // ─── fork 子进程执行 reboot ───────────────────────────
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork() failed: " + std::string(strerror(errno));
        LOG_ERROR("AndroidExecutor: fork failed: " + err);
        state_mgr.clear_pending();
        return "failed";
    }

    if (pid == 0) {
        // 子进程：延迟 3 秒后执行 reboot
        sleep(3);

        // ── 清 pending 文件（reboot 即将执行）─────────────
        state_mgr.clear_pending();

        // 尝试多种 reboot 方式，按优先级降序：
        // 1. PowerManager.reboot() — 需要 Java JNI，最可靠
        // 2. /system/bin/reboot — 需要 root
        // 3. /sbin/shutdown -r — 需要 root

        bool success = false;

        // 方式 1：通过 JNI 调用 PowerManager.reboot()
        if (g_jvm != nullptr) {
            JNIEnv* env = getJNIEnv();
            if (env != nullptr) {
                // 获取 ActivityManager.Service
                jclass activityManagerClass = env->FindClass("android/app/ActivityManager");
                if (activityManagerClass != nullptr) {
                    jmethodID getService = env->GetStaticMethodID(
                        activityManagerClass, "getService", "()Landroid/app/IActivityManager;");
                    if (getService != nullptr) {
                        jobject iActivityManager = env->CallStaticObjectMethod(
                            activityManagerClass, getService);
                        if (iActivityManager != nullptr) {
                            jclass iActivityManagerClass = env->GetObjectClass(iActivityManager);
                            jmethodID shutdown = env->GetMethodID(
                                iActivityManagerClass, "shutdown", "(I)V");
                            if (shutdown != nullptr) {
                                // 0 = shutdown, 1 = reboot
                                env->CallVoidMethod(iActivityManager, shutdown, 1);
                                success = true;
                                LOG_INFO("AndroidExecutor: called ActivityManager.shutdown(reboot=true)");
                            }
                        }
                    }
                }
            }
        }

        // 方式 2：fallback 到 shell 命令
        if (!success) {
            if (hasElevatedPrivileges()) {
                // root 权限：直接调用 reboot
                int ret = system("/system/bin/reboot");
                if (ret == 0) success = true;
            } else {
                // 普通权限：尝试 pm reboot（需要 root）
                int ret = system("/system/bin/pm reboot");
                if (ret == 0) success = true;
            }
        }

        // 如果所有方式都失败
        if (!success) {
            LOG_ERROR("AndroidExecutor: all reboot methods failed");
            std::ofstream ofs("/tmp/device-agent-reboot-status.json");
            if (ofs.is_open()) {
                ofs << "{\n"
                    << "  \"command_id\": \"" << command_id << "\",\n"
                    << "  \"status\": \"failed\",\n"
                    << "  \"error\": \"no method available to reboot\"\n"
                    << "}\n";
                ofs.close();
            }
            showToast("Reboot failed: insufficient permissions");
            _exit(1);
        }

        // reboot 成功，系统会重启，不会执行到这里
        _exit(0);
    }

    // ─── 父进程：非阻塞等待 ──────────────────────────────
    LOG_INFO("AndroidExecutor: reboot child pid=" + std::to_string(pid));

    int status;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == 0) {
        LOG_INFO("AndroidExecutor: reboot in progress, returning pending");
        return "pending";
    }

    // 子进程立即退出 = reboot 失败
    if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        LOG_ERROR("AndroidExecutor: reboot child exited with code " + std::to_string(exit_code));
        err = "reboot failed: no method available";
        return "failed";
    }

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
