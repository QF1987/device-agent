# DeviceOps 改进任务清单（执行指令）

> **本文档是给 openClaw（MiniMax 2.7）的执行指令**，不是普通设计文档。
> 每个任务都是**独立可执行、可验证、可单独 commit** 的工作单元。
> 严格按顺序执行；每完成一个任务单独 commit，然后再开始下一个。

---

## 📋 项目背景（必读）

- **仓库**：`device-agent`（设备端，C++/Kotlin 多平台）
- **配套仓库**：`terminal-agent`（服务端，Go，在 `/Users/qf/.openclaw/workspace/terminal-agent/`）
- **当前分支**：`main`
- **MVP 重点平台**：Android（小米 MIUI）
- **已知架构事实**（不要去改这些）：
  - Android 端**目前只跑 Kotlin HTTP polling**（`DeviceAgentService.CommandPoller`），每 10 秒拉一次 `GET /api/v1/devices/{id}/poll`
  - C++ `AndroidExecutor` 已实现 reboot / updateConfig / upgradeFirmware，但 **JNI 通道未接线**（Kotlin 端从未调 `System.loadLibrary` / `nativeStart`）
  - **JNI WIP 文件**：`src/jni_bridge.h` 和 `src/jni_wrapper.cpp` 当前是 git untracked 状态（有代码没 commit）
  - 桌面（Linux/macOS）走完整 C++ 通道（`DeviceClient` + `CommandHandler` + `LinuxExecutor/MacOSExecutor`）
  - APK 安装只走一条路：`handleCheckLocalApk` 里的 `FileProvider + Intent.ACTION_VIEW`（用户点确认）
  - 服务端地址硬编码：`android/app/src/main/java/com/deviceagent/DeviceAgentService.kt:21`

### 🎯 架构方向（必读，决定每个任务的边界）

- **当前目标（方案 A，本文档执行范围）**：让 Android 的 reboot / update_config 通过 **JNI → C++ `AndroidExecutor`** 执行。Kotlin `CommandPoller` 仍然负责 HTTP polling 拉命令，只在收到这两类命令时用 JNI 转发给 C++。**不激活** `nativeStart`（不建 `DeviceClient` + `CommandHandler`，gRPC streaming 通道保持未接线）。
- **最终目标（方案 B，不在本文档范围）**：Kotlin 完全停掉 `CommandPoller`，`onCreate` 调 `nativeStart`，C++ 接管 gRPC streaming + 命令分发，APK 安装等 UI 操作反向 JNI 回调 Kotlin。
- **为什么这么分**：方案 B 要先解决 prebuilt gRPC Android 连通性、文件下载反向回调等问题，是几周工作，和本文档的"补 reboot/update_config"不混在一起做。但**方案 A 的 JNI 骨架要为 B 铺路**，不要在 Kotlin 层另起一套 handler（那是技术债）。

---

## 🚫 必读约束（违反即停止）

1. **不要新增抽象层 / 接口 / 设计模式**。所有任务都是小手术，不是重构。
2. **不要 `pm install`、不要改动 installer-helper 来"修"它**。Task 3 要删除它。
3. **JNI 只做方案 A**：Task 2 需要改 `src/jni_wrapper.cpp` 和 Kotlin loadLibrary，但**只新增 `nativeDispatchCommand` 这一条路径**。**不要**在 Kotlin 里调 `nativeStart`，**不要**启动 `DeviceClient` / `CommandHandler`，**不要**改 gRPC 代码。这是刻意的边界 —— 方案 B 留给后续专题。
4. **不要动 `.proto` 文件**（Task 5 只写脚本，不改 proto 内容）。
5. **每完成一个 Task 单独 commit**；commit 前先跑该 Task 的"验收步骤"。
6. **遇到不确定的地方立即停止**，在当前任务末尾用 `⚠️ 需要用户确认` 标记问题，**不要猜**。
7. **不要写新的 .md 文档**（除非任务明确要求）。
8. **不要加注释解释"做了什么"**。代码自解释。
9. **Android 改动必须真机验证**，不能只靠编译通过（MIUI 行为和 AOSP 不同）。如果当前没有连接 Android 设备，在该步骤标注 `⚠️ 待真机验证`，先提交代码。

---

## 🔥 Task 1：服务端地址可配（必做，高优先级）

### 目标
`DeviceAgentService.kt` 里的 `serverUrl` 和 `deviceId` 目前硬编码。改成：
- **默认值**从 `BuildConfig` 来（Gradle 构建时注入）
- **运行时可覆盖**：Intent extra（方便 `adb` 调试）→ SharedPreferences 持久化

### 输入
- `android/app/src/main/java/com/deviceagent/DeviceAgentService.kt:21-22`
  ```kotlin
  private var serverUrl: String = "http://192.168.31.81:8080"
  private var deviceId: String = "ANDROID-001"
  ```
- `android/app/build.gradle`

### 执行步骤

**步骤 1.1**：在 `android/app/build.gradle` 的 `defaultConfig` 里加 BuildConfig 字段：

```gradle
defaultConfig {
    // ... 原有内容 ...
    buildConfigField "String", "DEFAULT_SERVER_URL", "\"http://192.168.31.81:8080\""
    buildConfigField "String", "DEFAULT_DEVICE_ID", "\"ANDROID-001\""
}
buildFeatures {
    buildConfig = true
}
```

**步骤 1.2**：在 `DeviceAgentService.kt` 顶部替换硬编码：

```kotlin
// 优先级：Intent extra > SharedPreferences > BuildConfig
private const val PREFS_NAME = "device_agent_config"
private const val KEY_SERVER_URL = "server_url"
private const val KEY_DEVICE_ID = "device_id"

private var serverUrl: String = ""
private var deviceId: String = ""
```

**步骤 1.3**：在 `DeviceAgentService.onCreate()` 最顶部（`mainServiceRef = WeakReference(this)` 之前）初始化：

```kotlin
val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
serverUrl = prefs.getString(KEY_SERVER_URL, null) ?: BuildConfig.DEFAULT_SERVER_URL
deviceId = prefs.getString(KEY_DEVICE_ID, null) ?: BuildConfig.DEFAULT_DEVICE_ID
```

**步骤 1.4**：在 `onStartCommand` 里加 Intent extra 覆盖逻辑：

```kotlin
override fun onStartCommand(i: Intent?, f: Int, s: Int): Int {
    i?.getStringExtra("server_url")?.let {
        serverUrl = it
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(KEY_SERVER_URL, it).apply()
        android.util.Log.i(TAG, "serverUrl overridden: $it")
    }
    i?.getStringExtra("device_id")?.let {
        deviceId = it
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putString(KEY_DEVICE_ID, it).apply()
        android.util.Log.i(TAG, "deviceId overridden: $it")
    }
    return START_STICKY
}
```

### 验收步骤

```bash
cd /Users/qf/.openclaw/workspace/device-agent/android
./gradlew :app:assembleDebug 2>&1 | tail -5
# 期望：BUILD SUCCESSFUL

adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am force-stop com.deviceagent
adb shell am startservice -n com.deviceagent/.DeviceAgentService --es server_url "http://10.0.0.1:9999" --es device_id "TEST-999"
sleep 2
adb logcat -d -s DeviceAgentService | grep "overridden" | tail -2
# 期望：看到两条 overridden 日志
```

### Commit
```
refactor(android): make serverUrl and deviceId configurable

- Add BuildConfig defaults for server URL and device ID
- Load from SharedPreferences at service startup
- Allow runtime override via Intent extras (persisted to prefs)

Replaces hardcoded http://192.168.31.81:8080 and ANDROID-001.
```

---

## 🔥 Task 2：方案 A — JNI 最小接通，reboot / update_config 走 C++ Executor（必做，高优先级）

### 目标
当前 Android 端 reboot / update_config **完全没有 handler**，收到会被忽略。C++ 侧 `AndroidExecutor` 已实现这两个方法（见 `src/executor/executor.h:113-127` 和 `src/executor/android_executor.cc`）。本任务**通过 JNI 把 Kotlin 的命令分发接到 C++ `AndroidExecutor`**。

**边界**（反复读一遍，别越界）：
- ✅ 允许：让 `System.loadLibrary("device-agent")` 跑起来触发 `JNI_OnLoad`；新增 `nativeDispatchCommand` 方法；Kotlin 侧 handler 只做 JSON 解析 + 转发
- ❌ 禁止：调 `nativeStart`；启动 `DeviceClient` / `CommandHandler`；任何 gRPC 代码路径
- ❌ 禁止：在 Kotlin 层用 `Runtime.exec("su", "-c", "reboot")` 直接做 reboot，或者在 Kotlin 里另建一套 SharedPreferences 写 config。**这是技术债**，已经踩过坑。所有执行动作走 C++ `AndroidExecutor`。

### 输入
- `src/jni_wrapper.cpp`（**当前是 git untracked**，本任务内把它变成 tracked + 扩展）
- `src/jni_bridge.h`（同上）
- `src/executor/executor.h:35-41`（`reboot` / `updateConfig` 接口签名）
- `src/executor/android_executor.cc`（**先读这个文件**，确认 `reboot` 和 `updateConfig` 的具体实现，尤其 `updateConfig` 实际做什么）
- `android/app/src/main/java/com/deviceagent/DeviceAgentService.kt` 的 `handleCmd()` 和 `CommandPoller`
- `CMakeLists.txt`（Android 分支）：**先读** `if(ANDROID)` 段（约 34-123 行），确认 `jni_wrapper.cpp` 是否已被包含到 `device-agent.so` 的 sources 里

### 执行步骤

**步骤 2.0（调研，不改代码）**：
1. 读 `src/executor/android_executor.cc`，找到 `AndroidExecutor::updateConfig` 实现，**确认它是否是 no-op**。如果是 no-op（只填 err 返回），**停下来**标注 `⚠️ 需要用户确认：updateConfig C++ 实现为空，是否要在此任务顺便补实现？`
2. 读 `CMakeLists.txt` 的 Android 分支，确认 sources 列表里是否包含 `src/jni_wrapper.cpp`。如果没有，加上（在 Android 分支的 sources 列表里追加一行）。
3. 读 `src/jni_wrapper.cpp:100`，确认 `env->RegisterNatives(cls, g_methods, 2)` 这里硬编码的 `2` 和数组实际长度（6）不一致 —— 这是一个已知 bug，本任务要顺便修掉。

**步骤 2.1**：修 `src/jni_wrapper.cpp` 的 `RegisterNatives` count bug，同时为新方法准备入口。

在 `g_methods[]` 数组**末尾**追加一项（注意签名字符串格式）：

```cpp
{
    const_cast<char*>("nativeDispatchCommand"),
    const_cast<char*>("(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;"),
    reinterpret_cast<void*>(&Java_com_deviceagent_DeviceAgentService_nativeDispatchCommand_impl)
}
```

把 `RegisterNatives` 调用改成：
```cpp
const jint method_count = sizeof(g_methods) / sizeof(g_methods[0]);
if (env->RegisterNatives(cls, g_methods, method_count) != 0) {
    LOGE("JNI_OnLoad: RegisterNatives failed");
    return JNI_ERR;
}
LOGI("JNI_OnLoad: registered %d native methods", method_count);
```

**步骤 2.2**：在 `src/jni_wrapper.cpp` 末尾新增 `nativeDispatchCommand` 实现。

先加前向声明（在文件顶部的前向声明区）：
```cpp
static jstring Java_com_deviceagent_DeviceAgentService_nativeDispatchCommand_impl(
    JNIEnv* env, jclass clazz, jstring jCmdType, jstring jPayloadJson);
```

然后文件末尾加实现。**注意**：这个函数只依赖 `g_jvm`（由 `JNI_OnLoad` 初始化），不依赖 `g_java_service`（那是 `nativeStart` 才建的）—— 所以方案 A 边界是自洽的。

```cpp
// ─── nativeDispatchCommand 实现（方案 A 入口）─────────────
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

    // 简易 JSON 字段提取（复用 command_handler 同风格；payload 是 Kotlin 扁平 JSON）
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
```

**⚠️ 注意**：
- 如果 `android_executor.cc` 里有现成的 JSON 解析工具（比如 nlohmann::json 或类似），**优先复用**，不要再写上面这个简易 extract。读 `src/client/command_handler.cc` 看现有风格。
- 如果发现 `updateConfig` 是 no-op（步骤 2.0 的发现），本任务**不要顺手给它补实现**，标注 `⚠️ 需要用户确认` 即可。

**步骤 2.3**：Kotlin 端 `DeviceAgentService.kt` 改动。

**3a. 在 `companion object` 里加 `loadLibrary` 和 external 方法声明**（如果已有 companion object 就追加；没有就新建）：

```kotlin
companion object {
    init {
        try {
            System.loadLibrary("device-agent")
            android.util.Log.i("DeviceAgentService", "native lib loaded")
        } catch (e: UnsatisfiedLinkError) {
            android.util.Log.e("DeviceAgentService", "loadLibrary failed: ${e.message}")
        }
    }

    @JvmStatic
    external fun nativeDispatchCommand(type: String, payload: String): String
}
```

**⚠️ 注意**：
- 如果 `DeviceAgentService` 里已经有 `mainServiceRef` 等 companion 成员，把上面的 `init` 和 `external fun` 合并进现有 companion，不要建第二个。
- `loadLibrary` 失败时只记日志**不 throw** —— 这样 JNI 没编进 `.so` 时（比如桌面环境/缺 ABI），APK 还是能起来，只是 reboot/update_config 会走 fallback（下面的 handler 会捕获）。

**3b. 在 `handleCmd` 的 `when` 块里加两个分支**：

```kotlin
when (type) {
    "DOWNLOAD_READY" -> handleDL(cmdId, p, svc)
    "UPGRADE_APP" -> handleUP(cmdId, p, svc)
    "CHECK_LOCAL_APK" -> handleCheckLocalApk(cmdId, p, svc)
    "REBOOT", "reboot" -> handleReboot(cmdId, p, svc)
    "UPDATE_CONFIG", "update_config" -> handleUpdateConfig(cmdId, p, svc)
    else -> android.util.Log.w(TAG, "unknown cmd type: $type")
}
```

**3c. 实现 `handleReboot`**（在 `CommandPoller` 对象里，`handleCheckLocalApk` 下面）：

```kotlin
private fun handleReboot(cmdId: String, p: JSONObject, svc: Service) {
    android.util.Log.i(TAG, "REBOOT cmd=$cmdId payload=$p")
    // 先回报 executing，否则重启后没机会回报
    reportCS(cmdId, "executing", "Reboot initiated via JNI")
    Thread {
        try {
            val payload = JSONObject().apply {
                put("force", p.optBoolean("force", false))
                put("command_id", cmdId)
            }
            val result = nativeDispatchCommand("reboot", payload.toString())
            android.util.Log.i(TAG, "reboot native result: $result")
            val obj = JSONObject(result)
            val status = obj.optString("status", "failed")
            val msg = obj.optString("message", "")
            // status=="pending" 表示 C+D 已写 pending 文件并 fork 子进程，无需此处回报
            if (status != "pending") {
                reportCS(cmdId, status, msg)
            }
        } catch (e: Throwable) {
            android.util.Log.e(TAG, "handleReboot failed: ${e.message}")
            reportCS(cmdId, "failed", "JNI dispatch error: ${e.message}")
        }
    }.start()
}
```

**3d. 实现 `handleUpdateConfig`**：

```kotlin
private fun handleUpdateConfig(cmdId: String, p: JSONObject, svc: Service) {
    val key = p.optString("key", "")
    val value = p.optString("value", "")
    android.util.Log.i(TAG, "UPDATE_CONFIG cmd=$cmdId key=$key")
    if (key.isEmpty()) {
        reportCS(cmdId, "failed", "Empty key")
        return
    }
    try {
        val payload = JSONObject().apply {
            put("key", key)
            put("value", value)
        }
        val result = nativeDispatchCommand("update_config", payload.toString())
        android.util.Log.i(TAG, "update_config native result: $result")
        val obj = JSONObject(result)
        reportCS(cmdId, obj.optString("status", "failed"), obj.optString("message", ""))
    } catch (e: Throwable) {
        android.util.Log.e(TAG, "handleUpdateConfig failed: ${e.message}")
        reportCS(cmdId, "failed", "JNI dispatch error: ${e.message}")
    }
}
```

**⚠️ 注意**：
- 这两个 handler **不碰 Kotlin 侧 SharedPreferences**。Task 1 的 `serverUrl` 持久化是独立路径（用于应用启动时的本地配置），update_config 命令走 C++ Executor 是"远程配置下发"，两条路的语义不能混。如果 C++ `updateConfig` 是 no-op，这件事要到后续专题补。
- 不要**加 `allowedKeys` 白名单在 Kotlin 层**。白名单应该在 C++ `AndroidExecutor::updateConfig` 里做，Kotlin 只做透传（否则未来改白名单要改两边）。

### 验收步骤

```bash
cd /Users/qf/.openclaw/workspace/device-agent/android
./gradlew :app:assembleDebug 2>&1 | tail -20
# 期望：BUILD SUCCESSFUL

adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -c
adb shell am force-stop com.deviceagent
adb shell am startservice com.deviceagent/.DeviceAgentService
sleep 3
adb logcat -d | grep -E "(DeviceAgentJNI|DeviceAgentService)" | head -30
# 期望至少看到：
#   DeviceAgentService: native lib loaded
#   DeviceAgentJNI: JNI_OnLoad: registered 7 native methods
```

**真机功能验证（如果当前有 terminal-agent 和设备）**：
1. 启动 `terminal-agent-serve`
2. 用 CLI 下发 `update_config` 命令（key=例如 test_key, value=test_val）
3. 观察 logcat：应看到 `nativeDispatchCommand: type=update_config ...` 和服务端 DB 的 command 状态变 `completed` 或 `failed`
4. `reboot` 命令**不要在开发机真验证**（会真重启），改成只确认 `nativeDispatchCommand: type=reboot` 日志出现就行，或者 dry-run 一下（目前 C++ 端 reboot 会写 pending 文件 + fork —— 不要真跑）

如果没真机，标注 `⚠️ 待真机验证 Task 2 命令路径`。

### Commit
```
feat(android): wire reboot/update_config to C++ AndroidExecutor via JNI (scheme A)

Previously only 3 of 5 command types were implemented on Android. The C++
AndroidExecutor already implements reboot/updateConfig, but the JNI channel
was never wired up. This change adds the minimum JNI path:

- Track src/jni_wrapper.cpp and src/jni_bridge.h (were untracked WIP)
- Fix RegisterNatives count bug (was hardcoded 2, now matches array size)
- Add nativeDispatchCommand(type, payloadJson) → new C++ entry point that
  instantiates AndroidExecutor and dispatches reboot / update_config
- Kotlin: System.loadLibrary + external fun declaration, handleReboot /
  handleUpdateConfig forward via JNI (no business logic in Kotlin)

Scheme A boundary: nativeStart / DeviceClient / CommandHandler remain
unwired — gRPC streaming on Android is a future milestone (scheme B).
```

---

## 🔥 Task 3：清理 legacy 代码（必做，高优先级）

### 目标
两段历史包袱：
- `android/installer-helper/` 整个模块（独立 APK，MIUI 权限早期探索，未被使用）
- `DeviceAgentService.kt` 里的 `runPmInstall()` 函数（`pm install -r`，MIUI 会 exitCode=255，已被 Intent 方式取代）

### 执行步骤

**步骤 3.1**：先给当前 HEAD 打 archive tag（保留历史）：
```bash
git tag archive/installer-helper-before-removal
```

**步骤 3.2**：删除整个 installer-helper 模块：
```bash
rm -rf /Users/qf/.openclaw/workspace/device-agent/android/installer-helper/
```

**步骤 3.3**：编辑 `android/settings.gradle`，移除 `':installer-helper'`：
```bash
# 找到类似 include ':app', ':installer-helper' 的行，改为 include ':app'
```

**步骤 3.4**：找到 `runPmInstall` 函数及所有调用：
```bash
cd /Users/qf/.openclaw/workspace/device-agent
grep -rn "runPmInstall" android/app/src/
```
- 如果有调用方，**停止**并标注 `⚠️ 需要用户确认`（此前 agent 确认过没调用，但再次核实）
- 如果没调用方，直接从 `DeviceAgentService.kt` 删除该函数定义

**步骤 3.5**：编译验证：
```bash
cd /Users/qf/.openclaw/workspace/device-agent/android
./gradlew :app:assembleDebug 2>&1 | tail -10
# 期望：BUILD SUCCESSFUL
```

**步骤 3.6**：安装并确认现有功能正常（APK 安装、命令轮询）：
```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb logcat -c && adb shell am force-stop com.deviceagent && \
  adb shell am startservice com.deviceagent/.DeviceAgentService && \
  sleep 15 && adb logcat -d -s DeviceAgentService CommandPoller | tail -20
# 期望：CommandPoller 在轮询，没有异常
```

### Commit
```
chore: remove installer-helper module and unused runPmInstall()

Both were early attempts at MIUI install permission workarounds that
never became active:
- installer-helper: separate APK (com.deviceagent.helper) for a side-channel
  install flow; superseded by FileProvider + Intent.ACTION_VIEW
- runPmInstall(): Runtime.exec("pm install -r"); MIUI blocks this (needs
  MANAGE_USERS system signature), always returns exitCode 255

History preserved at tag archive/installer-helper-before-removal.
```

---

## ⚙️ Task 4：最小化端到端集成测试（中优先级）

### 目标
**一个脚本**验证命令闭环：CLI 下发 → DB 记录 → 设备收到 → 设备回报 → DB 状态更新。
不是单元测试；是防回归的烟测。

### 执行步骤

**步骤 4.1**：创建文件 `/Users/qf/.openclaw/workspace/device-agent/tests/smoke_command_roundtrip.sh`

```bash
#!/usr/bin/env bash
# 端到端冒烟测试：CLI 下发命令 → 模拟设备 poll/report → 验证 DB 状态
# 前置：terminal-agent-serve 正在跑（localhost:8080）、Postgres 可连
set -euo pipefail

TERMINAL_AGENT="/Users/qf/.openclaw/workspace/terminal-agent"
DEVICE_ID="SMOKE-TEST-$(date +%s)"
SERVE_URL="http://localhost:8080"

cleanup() {
    # 删除测试设备
    PGPASSWORD=deviceops123 psql -h localhost -U deviceops -d deviceops \
        -c "DELETE FROM commands WHERE device_id='$DEVICE_ID'; DELETE FROM devices WHERE id='$DEVICE_ID';" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "==> 1. 注册测试设备"
PGPASSWORD=deviceops123 psql -h localhost -U deviceops -d deviceops -c \
    "INSERT INTO devices (id, name, region, status, version) VALUES ('$DEVICE_ID', 'smoke-test', 'test', 'online', '1.0.0');"

echo "==> 2. CLI 下发 update_config 命令"
cd "$TERMINAL_AGENT"
./bin/device-ctl batch config "$DEVICE_ID" --key poll_interval_ms --value 5000 --yes
# 或者用 curl 直接打 HTTP API，取决于 CLI 实际接口

echo "==> 3. 查询 DB 确认 pending"
STATUS=$(PGPASSWORD=deviceops123 psql -h localhost -U deviceops -d deviceops -tAc \
    "SELECT status FROM commands WHERE device_id='$DEVICE_ID' ORDER BY id DESC LIMIT 1;")
[[ "$STATUS" == "pending" ]] || { echo "FAIL: status=$STATUS expected pending"; exit 1; }

echo "==> 4. 模拟设备 poll"
POLL_RESP=$(curl -s "$SERVE_URL/api/v1/devices/$DEVICE_ID/poll")
CMD_ID=$(echo "$POLL_RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['commands'][0]['command_id']) if d.get('commands') else exit(1)")
[[ -n "$CMD_ID" ]] || { echo "FAIL: no command_id in poll response"; exit 1; }

echo "==> 5. 模拟设备回报 completed"
curl -s -X POST "$SERVE_URL/api/v1/devices/$DEVICE_ID/report" \
    -H "Content-Type: application/json" \
    -d "{\"command_id\":\"$CMD_ID\",\"status\":\"completed\",\"result_message\":\"smoke test ok\"}"

echo "==> 6. 查询 DB 确认 completed"
sleep 1
STATUS=$(PGPASSWORD=deviceops123 psql -h localhost -U deviceops -d deviceops -tAc \
    "SELECT status FROM commands WHERE command_id='$CMD_ID';")
[[ "$STATUS" == "completed" ]] || { echo "FAIL: status=$STATUS expected completed"; exit 1; }

echo "==> ✅ PASS"
```

**步骤 4.2**：加执行权限：
```bash
chmod +x /Users/qf/.openclaw/workspace/device-agent/tests/smoke_command_roundtrip.sh
```

**步骤 4.3**：在 `device-agent/Makefile` 加 target（如果没有 Makefile 就创建）：
```makefile
.PHONY: smoke
smoke:
	@bash tests/smoke_command_roundtrip.sh
```

### 验收步骤

```bash
# 前置：先启动 terminal-agent-serve
cd /Users/qf/.openclaw/workspace/terminal-agent && ./terminal-agent-serve &
SERVE_PID=$!
sleep 2

# 跑烟测
cd /Users/qf/.openclaw/workspace/device-agent
make smoke
# 期望：==> ✅ PASS

kill $SERVE_PID
```

**⚠️ 可能问题**：
- `device-ctl batch config` 的具体命令行参数可能和示例不符，需要先 `./bin/device-ctl batch config --help` 确认。**如果参数不匹配，先停下来标注 `⚠️ 需要用户确认 device-ctl 参数`**，不要瞎猜。
- Postgres 密码如果和环境不符（默认 `deviceops123`），同样标注确认。

### Commit
```
test: add end-to-end smoke test for command round-trip

Single script verifying: CLI dispatch → DB pending → device poll → device report → DB completed.
This is the minimum viable regression gate for the core command flow.
```

---

## ⚙️ Task 5：写 `scripts/gen-proto.sh` 脚本（中优先级）

### 目标
`gen-android/` 用 protoc v4.25.1，`gen-desktop/` 用 v34.1。**版本统一是独立专题**（后续要升 Android prebuilt gRPC），本任务**不做统一**，只写一个脚本一键跑两个版本的 protoc，避免改 `.proto` 时漏更新一边。

**边界**：
- ❌ 不改 `.proto` 内容
- ❌ 不动 `gen-android/` / `gen-desktop/` 现有生成结果
- ❌ 不碰 prebuilt gRPC、不升级版本
- ✅ 只新增 `scripts/gen-proto.sh` 和可选的 Makefile target

### 执行步骤

**步骤 5.1（调研）**：先查清楚两个 protoc 实际怎么跑的。

1. 读 `gen-android/terminal_agent/v1/device.pb.h` 顶部注释确认 protoc 版本
2. 读 `gen-desktop/terminal_agent/v1/device.pb.h` 顶部注释确认 protoc 版本
3. 查仓库里有没有现成的 proto 生成脚本或 Makefile target（`grep -r "protoc" --include="Makefile" --include="*.sh"`）。如果有，就**基于它扩展**，不要另起一套
4. 查 `proto/terminal_agent/v1/*.proto` 文件列表
5. 如果找不到两个版本 protoc 的实际路径（用户机器上 protoc v4.25.1 和 v34.1 各自在哪），**停下来标注** `⚠️ 需要用户确认两个 protoc 版本的路径`，不要瞎猜 `/usr/local/bin/protoc` 之类

**步骤 5.2**：创建 `scripts/gen-proto.sh`。模板如下（具体路径在步骤 5.1 确认后再定）：

```bash
#!/usr/bin/env bash
# 一键生成 Android + 桌面两套 protobuf/gRPC 代码
# Android 用 protoc v4.25.1 (配合 protobuf-javalite)
# 桌面   用 protoc v34.1
#
# 环境变量覆盖（可选）：
#   PROTOC_ANDROID=/path/to/protoc-4.25.1
#   PROTOC_DESKTOP=/path/to/protoc-34.1
#   GRPC_CPP_PLUGIN_ANDROID=/path/to/grpc_cpp_plugin  (对应 Android 侧 prebuilt gRPC)
#   GRPC_CPP_PLUGIN_DESKTOP=/path/to/grpc_cpp_plugin  (对应 /opt/homebrew/Cellar/grpc/...)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PROTO_DIR="$REPO_ROOT/proto"
GEN_ANDROID="$REPO_ROOT/gen-android"
GEN_DESKTOP="$REPO_ROOT/gen-desktop"

# 默认值由步骤 5.1 调研确定后填入；若调研时未确定，用环境变量强制外部传入
: "${PROTOC_ANDROID:?Set PROTOC_ANDROID to protoc v4.25.1 binary}"
: "${PROTOC_DESKTOP:?Set PROTOC_DESKTOP to protoc v34.1 binary}"
: "${GRPC_CPP_PLUGIN_ANDROID:?Set GRPC_CPP_PLUGIN_ANDROID}"
: "${GRPC_CPP_PLUGIN_DESKTOP:?Set GRPC_CPP_PLUGIN_DESKTOP}"

PROTO_FILES=$(find "$PROTO_DIR" -name "*.proto")

echo "==> generating Android protos into $GEN_ANDROID (protoc=$PROTOC_ANDROID)"
mkdir -p "$GEN_ANDROID"
"$PROTOC_ANDROID" \
    --proto_path="$PROTO_DIR" \
    --cpp_out="$GEN_ANDROID" \
    --grpc_out="$GEN_ANDROID" \
    --plugin=protoc-gen-grpc="$GRPC_CPP_PLUGIN_ANDROID" \
    $PROTO_FILES

echo "==> generating Desktop protos into $GEN_DESKTOP (protoc=$PROTOC_DESKTOP)"
mkdir -p "$GEN_DESKTOP"
"$PROTOC_DESKTOP" \
    --proto_path="$PROTO_DIR" \
    --cpp_out="$GEN_DESKTOP" \
    --grpc_out="$GEN_DESKTOP" \
    --plugin=protoc-gen-grpc="$GRPC_CPP_PLUGIN_DESKTOP" \
    $PROTO_FILES

echo "==> ✅ done"
```

**⚠️ 注意**：
- 上面脚本 `--cpp_out` / `--grpc_out` 是 C++ 侧。如果 Android 还需要生成 Java `--java_out`（配合 `protobuf-javalite`），**先读现有 gen-android 目录是否有 .java 文件**再决定是否加 Java 出参。如果 gen-android 没有 .java（即 Java 侧走 Gradle protobuf 插件自动生成），**不要加 `--java_out`**
- 如果仓库有现成 proto 生成脚本，**改它而不是新建**；改动后保留旧脚本的 flag（比如 grpc plugin 路径）
- 路径占位符（`PROTOC_ANDROID` 等）如果在步骤 5.1 确认了默认值，可以在脚本里加 `PROTOC_ANDROID="${PROTOC_ANDROID:-/实际默认路径}"` 兜底；**调研时没确认的绝对不要编造默认值**

**步骤 5.3**：加执行权限 + Makefile target：

```bash
chmod +x scripts/gen-proto.sh
```

在 `Makefile` 里加（如果 Makefile 不存在就不要为此建；注意根目录 Makefile 还是 android/Makefile？优先根目录）：

```makefile
.PHONY: proto
proto:
	@bash scripts/gen-proto.sh
```

**步骤 5.4**：在 `docs/system-overview.md` 或类似总览文档**不要**加说明（约束 7：不写新 md）。脚本自身的 header 注释已说清用法。

### 验收步骤

```bash
cd /Users/qf/.openclaw/workspace/device-agent

# 先跑一次，验证脚本能跑通
PROTOC_ANDROID=/path/to/4.25.1/protoc \
PROTOC_DESKTOP=/path/to/34.1/protoc \
GRPC_CPP_PLUGIN_ANDROID=/path/to/android/grpc_cpp_plugin \
GRPC_CPP_PLUGIN_DESKTOP=/path/to/desktop/grpc_cpp_plugin \
bash scripts/gen-proto.sh

# 确认两个目录都有文件更新
git status gen-android/ gen-desktop/
# 期望：两个目录都显示 modified（或没变化，因为内容幂等 —— 只要没有 error 即可）
```

**⚠️ 如果实际运行时 protoc 路径未知，跳过验收、只提交脚本，标注** `⚠️ 待用户提供两个 protoc 路径后实测`。

### Commit
```
build: add scripts/gen-proto.sh to regenerate both android/desktop proto sets

Android uses protoc v4.25.1 (protobuf-javalite compatible) and desktop uses
v34.1 — they live in gen-android/ and gen-desktop/ respectively. Previously
it was easy to update one and forget the other. This script drives both in
a single invocation.

Version unification (upgrading Android prebuilt gRPC) is a separate future
milestone; this task only wires up the two-version build.
```

---

## 🧹 Task 6 / Task 7（低优先级，暂不执行）

- **Task 6**：结构化日志（`log.Printf` → `zap`/`slog`）。涉及全 Go 仓库，改动面大，MVP 阶段不做。
- **Task 7**：DB migration 工具（`golang-migrate` 或 `goose`）。当前 schema 简单够用，等 schema 变化频繁时再做。

**如果完成了 Task 1-5，不要主动执行 Task 6-7**，等用户明确指示。

---

## 📝 每个 Task 完成后的固定流程

1. 跑该 Task 的"验收步骤"
2. 如果验收失败，**停下来**，不要强推
3. 验收通过后，单独 commit（用指定的 commit message 模板，可以微调但保留核心信息）
4. **不要**把多个 Task 合并 commit
5. **不要**在这一 Task 的 commit 里附带无关改动（比如顺手格式化其他文件）
6. 全部任务完成后，输出一个简短的 final report，列出：
   - 已完成的 Task 编号
   - 每个 Task 的 commit hash
   - 遇到的 `⚠️` 标记及具体内容
   - 用户需要确认的事项

---

## 🎯 开始执行

从 **Task 1** 开始，顺序执行到 **Task 5**。遇到 `⚠️` 立即停止并汇报。
