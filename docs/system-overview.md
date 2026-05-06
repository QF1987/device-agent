# DeviceOps 技术体系总览

> 目标：让 AI Agent 理解 DeviceOps 的完整架构、技术决策和设计理念，能参与方案讨论和问题诊断。

---

## 一、定位与核心理念

### DeviceOps 是什么

**DeviceOps（Device Operations）** = 设备自治系统。

核心理念：从"人管设备" → "设备自管理"。不是某个工具，是"设备要自治"的理念。

**市场空白**：
- MDM（Mobile Device Management）：只会控制，不会决策
- IoT 平台：只会采集数据，不会管理
- **DeviceOps**：设备能接收指令、执行、回报、主动上报，形成完整的控制闭环

### 三个目标场景

| 场景 | 说明 |
|------|------|
| 企业终端管理 | 自助购药机、充电桩、广告屏 |
| 家庭智能终端 | 智能家居设备、TV、路由器 |
| 机器人管理 | 巡检机器人、配送机器人 |

> ⚠️ 自助购药机只是 **MVP 示例**，不要局限在这个场景。

### 战略定位

- **短期**：开源建立影响力 → 场景切入
- **长期**：DeviceOps + AI = 决策层（策略优化、异常理解、自动恢复）
- **不适合**：一上来就创业，先建语言和范式

---

## 二、系统架构

```
┌─────────────────────────────────────────────────────────┐
│                      运维人员 / AI Agent                   │
│                    device-ctl CLI / CDP                   │
└─────────────────────┬───────────────────────────────────┘
                      │ gRPC / HTTP
┌─────────────────────▼───────────────────────────────────┐
│                   terminal-agent (Go)                     │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │
│  │  gRPC Server │  │ HTTP Server  │  │ File Server  │   │
│  │  (:9090)     │  │  (:8080)     │  │  文件分发     │   │
│  └──────────────┘  └──────────────┘  └──────────────┘   │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │              ConnectionManager                    │   │
│  │   • gRPC streaming (device → serve)              │   │
│  │   • HTTP polling (device → serve)                │   │
│  └─────────────────────────────────────────────────┘   │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────┐ │
│  │  batch   │  │ upgrade_ │  │  reboot   │  │ config │ │
│  │  reboot  │  │   app    │  │          │  │        │ │
│  └──────────┘  └──────────┘  └──────────┘  └────────┘ │
└─────────────────────┬───────────────────────────────────┘
                      │ gRPC CommandStream / HTTP
┌─────────────────────▼───────────────────────────────────┐
│                   device-agent (C++/Kotlin)               │
│                        (多平台)                            │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │         CommandHandler / CommandPoller            │   │
│  │   • 拉取命令 (HTTP polling / gRPC streaming)     │   │
│  │   • 下发执行                                     │   │
│  │   • 回报结果                                     │   │
│  └─────────────────────────────────────────────────┘   │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐   │
│  │   Executor    │  │ RebootState  │  │MetricsCol    │   │
│  │  (平台抽象)   │  │  Manager     │  │ lector       │   │
│  └──────────────┘  └──────────────┘  └──────────────┘   │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐                     │
│  │  Android     │  │   macOS      │                     │
│  │  Executor    │  │  Executor    │                     │
│  └──────────────┘  └──────────────┘                     │
└─────────────────────────────────────────────────────────┘
```

---

## 三、terminal-agent（服务端，Go）

> 仓库：`~/.openclaw/workspace/terminal-agent/`

### 核心职责

1. **接收命令**：CLI / AI Agent 下发指令
2. **转发设备**：通过 gRPC / HTTP 把命令送到设备
3. **文件分发**：HTTP Range 断点续传，支持多设备分发
4. **状态存储**：PostgreSQL，存储设备表 + 命令表

### CLI 命令

```bash
device-ctl list                        # 列出设备
device-ctl batch reboot <device-id>    # 重启设备
device-ctl batch config <device-id>    # 修改配置
device-ctl batch upgrade_app <device-id> --apk ./app.apk  # 升级 APK
device-ctl check_apk <device-id>       # 本地 APK 安装（MIUI 弹框确认）
device-ctl check_apk <device-id> --path /data/local/tmp/  # 指定路径
device-ctl release upload <file>       # 上传升级包
device-ctl release start <batch-id>    # 启动批次升级
device-ctl serve                       # 启动服务端
```

### 数据库表

**devices 表**：
| 字段 | 说明 |
|------|------|
| id | 设备 ID（如 ANDROID-001） |
| name | 设备名称 |
| region | 区域 |
| status | online/offline/error/maintenance |
| version | 当前版本 |

**commands 表**：
| 字段 | 说明 |
|------|------|
| id | 内部编号（CMD-数字） |
| command_id | UUID（与 proto 一致） |
| device_id | 目标设备 |
| command_type | 命令类型（reboot/update_config/upgrade_app/check_local_apk） |
| status | pending/sent/completed/failed/timeout |
| payload_json | 指令参数 |
| result_message | 执行结果 |

### 命令状态机

```
pending → sent → completed
                → failed
                → timeout
```

### 文件分发（Phase 1）

```
CLI: device-ctl release upload <apk>
     → serve HTTP POST /api/v1/files/upload
     → 保存到 /Users/qf/Alcedo/agent/files/

CLI: device-ctl release start <batch-id>
     → serve 记录 release_batch + release_batch_devices

设备 polling → 收到 DOWNLOAD_READY 命令
     → 设备从 serve HTTP 下载（Range 断点续传）
     → 完成 → 回报 seed_info（file_id, ip, port）
     → 后续设备优先从种子 HTTP 下载
```

---

## 四、device-agent（设备端，多平台 C++/Kotlin）

> 仓库：`~/.openclaw/workspace/device-agent/`

### 多平台架构

**共仓库 + 条件编译**，所有平台同一套代码：

```
device-agent/
├── src/
│   ├── executor/           # 平台执行器抽象
│   │   ├── executor.h      # 纯虚接口
│   │   ├── linux_executor.cc
│   │   ├── macos_executor.cc
│   │   ├── android_executor.cc
│   │   └── windows_executor.cc   # 待实现
│   ├── reboot_state/       # C+D 重启可靠性方案
│   │   ├── reboot_state.h
│   │   └── reboot_state.cc
│   ├── command_handler.cc  # 跨平台命令处理
│   └── main.cpp           # 入口（自动选择 Executor）
├── android/
│   └── app/src/main/java/com/deviceagent/
│         ├── DeviceAgentService.kt  # 前台 Service
│         ├── MainActivity.kt        # 透明跳板
│         ├── BootReceiver.kt        # 开机自启动
│         └── DownloadManager.kt     # APK 下载管理
└── CMakeLists.txt
```

**Android 特有实现**：
- Kotlin Service：接收命令、调度 Executor
- JNI 调用 C++ Executor
- Android 权限管理

### Executor 接口（平台抽象）

```cpp
class Executor {
public:
    virtual ~Executor() = default;

    // 重启设备
    // force=true 时强制重启
    // command_id: 用于 C+D 方案的 pending 文件名
    // 返回: "pending" / "success" / "failed"
    virtual std::string reboot(bool force, const std::string& command_id, std::string& err) = 0;

    // 更新配置
    virtual bool update_config(const std::string& key, const std::string& value, std::string& err) = 0;

    // 升级固件
    virtual bool upgrade_firmware(const std::string& url, const std::string& md5, std::string& err) = 0;

    // 升级应用（Android 特有）
    virtual bool upgrade_app(const std::string& apk_path, const std::string& md5, std::string& err) = 0;
};
```

### C+D 重启可靠性方案

**问题**：reboot 成功后进程被内核杀掉，无法主动回报结果。

**解决**：
- **C（命令级）**：fork 子进程 → 子进程清 pending 文件 → 执行 reboot
  - 成功：系统重启，pending 已清
  - 失败：子进程退出，服务端等待重连超时判失败
- **D（服务端）**：发命令后标记 pending → 设备重连则清除 pending → 超时则告警

### Android 端 APK 升级路径

**MIUI 安装拦截问题**：MIUI 系统级的 `pm install` 需要 `MANAGE_USERS` 权限（系统签名才给），app 调用 exitCode=255。

**正确路径（已验证）**：
```
APK → /sdcard/Android/data/com.deviceagent/files/DeviceAgent/Apk/
    → FileProvider URI
    → Intent.ACTION_VIEW（触发 MIUI 安装确认框）
    → 用户点确认 → 安装完成
```

**不需要任何存储权限**——私有外部存储路径 app 天然有完整访问权限。

### Android 端命令协议

```kotlin
// 命令类型
CHECK_LOCAL_APK   // 检查本地 APK 并安装
UPGRADE_APP       // HTTP 下载安装
DOWNLOAD_READY    // 批次下载指令
reboot            // 重启
update_config     // 修改配置

// 设备回报
POST /api/v1/devices/{device_id}/report
{
  "command_id": "uuid-string",
  "status": "completed|failed|installing",
  "result_message": "Installed version 38"
}
```

---

## 五、Command ID 设计

> ⚠️ 重要：曾因 Command ID 用错导致命令状态无法更新，这个设计值得注意。

数据库中命令有**两个 ID**：

| 字段 | 格式 | 来源 | 用途 |
|------|------|------|------|
| `id` | `CMD-数字` | 服务端生成 | DB 主键 |
| `command_id` | UUID | uuid.New() | 与 proto 一致，设备回报用这个 |

**教训**：Serve 的 HTTP Poll 响应必须返回 `command_id`（UUID），不能返回 `id`（CMD-数字）。设备用 UUID 回报，服务端用 UUID 匹配 `UpdateCommandResultByUUID()`。

---

## 六、known Issues & Solutions

### MIUI 安装弹框

- **现象**：Intent 安装触发 MIUI 安装确认框
- **根因**：MIUI 系统定制，第三方 app 安装必须用户确认
- **解法**：FileProvider + 私有外部存储路径，用户点一次确认后后续可静默

### MainActivity 被反复启动

- **现象**：每 10 秒闪一下
- **根因**：Mac 上的孤儿进程（`while true; do adb shell am start...`）在循环触发
- **解法**：`Theme.NoDisplay` 让 Activity 不创建 window
- **命令**：`pkill -f "am start -n com.deviceagent"`

### 命令状态一直是 sent 不变

- **根因**：Serve poll 响应的 `command_id` 用的是 `id`（CMD-数字）而不是 `command_id`（UUID）
- **解法**：SQL 查询加 `command_id` 列，改用 `UpdateCommandResultByUUID()`

---

## 七、设计原则

### CLI 设计

- **默认：单独 + 批量双支持**
  - 单独设备：`push_command`（即时响应）
  - 批量设备：`batch`（并发处理）
- **例外：按需裁剪**——如果某命令单设备场景无意义，只提供 batch 版本

### 命令命名

- Go 端：`CommandType` 常量 = 字符串（小写加下划线）
  - `check_local_apk` / `upgrade_app` / `reboot` / `update_config`
- Proto：独立 enum 或 string

### 平台差异处理

- C++ 核心跨平台，Platform 特有逻辑在 Executor 接口里
- Android：Kotlin + JNI + C++，注意 SELinux 权限
- macOS：fork 子进程执行特权操作

---

## 八、相关文档

| 文档 | 说明 |
|------|------|
| `docs/architecture.md` | device-agent 详细架构（含 Executor 接口定义） |
| `docs/troubleshooting-mainactivity-loop.md` | MainActivity 循环问题排查记录 |
| `terminal-agent/docs/claude-code-architecture-analysis.md` | Claude Code 架构分析（借鉴） |
| `terminal-agent/terminal-system-context.md` | DeviceOps 战略定位 |
| `/Users/qf/Alcedo/devops/文件分发系统研发任务清单与里程碑.md` | 文件分发系统研发计划 |
