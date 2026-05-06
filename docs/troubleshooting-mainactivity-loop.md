# 排查笔记：MainActivity 每 10 秒被反复启动

> 一次真实的定位过程，从"代码看起来没问题"到"揪出孤儿后台脚本"。

## 现象

在真机（小米 10 Pro，MIUI）上跑 `com.deviceagent`，每 10 秒左右就会看到一次 `MainActivity.onCreate` 被触发，app 被短暂置前再回后台。明明 manifest 里 MainActivity 没有 `MAIN`/`LAUNCHER` 的 intent-filter，不应该被"常规"入口拉起。

## 第一步：看代码，建立假设

先读 [AndroidManifest.xml](../android/app/src/main/AndroidManifest.xml) 和 [MainActivity.kt](../android/app/src/main/java/com/deviceagent/MainActivity.kt)：

- `MainActivity`：`exported=true`、**无 intent-filter**、`Theme.NoDisplay`、`onCreate` 里立刻 `startService()` 然后 `finish()`。
- `InstallTestActivity`：有 `<action android:name="android.intent.action.MAIN" />` 但**没有 `category.LAUNCHER`**。

结论：MainActivity 只能被**显式 Intent** 拉起（`am start -n com.deviceagent/.MainActivity` 或别的 app 用 `ComponentName` 显式指定）。既然不是自动入口，那一定是**谁在显式调用它**。

**关键技巧**：代码里 `onCreate` 已经打印了 caller：

```kotlin
val caller = callingActivity?.className ?: "system"
Log.i(TAG, "onCreate called by: $caller, intent: ${intent?.action ?: "none"}")
```

注意：`callingActivity` 为 null 时打印 "system"，**不代表真的是系统**，很多情况（比如 `startActivity` 带 `NEW_TASK` flag、或 `am start`）也会是 null。所以这行日志能看到"有调用"，但还得继续挖。

## 第二步：adb logcat 拿到"谁在拉"

连上设备，抓 ActivityTaskManager 的日志（Android 13+ 用 `ActivityTaskManager`，旧版本可能叫 `ActivityManager`）：

```bash
adb logcat -d 2>&1 | grep -E "ActivityTaskManager.*START.*deviceagent" | tail -30
```

结果：

```
04-19 20:59:36.928  I ActivityTaskManager: START u0 {flg=0x10000000 cmp=com.deviceagent/.MainActivity} \
    from uid 2000 from pid 4717 callingPackage com.android.shell
04-19 20:59:47.117  ... from pid 4756 callingPackage com.android.shell
04-19 20:59:57.311  ... from pid 4813 callingPackage com.android.shell
04-19 21:00:07.505  ... from pid 4869 callingPackage com.android.shell
...
```

三条关键信息：

| 字段 | 值 | 含义 |
|---|---|---|
| `callingPackage` | `com.android.shell` | **adb shell** 执行的 `am` 命令（不是设备上任何 app 自己发起的） |
| `uid` | `2000` | shell 用户 uid |
| `flg` | `0x10000000` | `FLAG_ACTIVITY_NEW_TASK`，典型 `am start` 行为 |
| `pid` | 每次不同（4717→4756→4813…） | 每次都是**新建**的短命进程 → 是 `adb shell am start ...` 一次性调用 |
| 时间间隔 | **精确 10 秒** | host 端有定时循环 |

此时假设已经很明确了：**host 上有东西在循环执行 `adb shell am start`**。

## 第三步：在 host 上找循环进程

### 尝试 1：直接 `ps` 找长期存活的 adb

```bash
ps -ef | grep -iE "adb|am start" | grep -v grep
```

只看到 adb server、`adb logcat`、scrcpy —— 没有循环。为什么？因为 `adb shell am start` 的生命周期只有几百毫秒，`ps` 很难刚好抓到。

### 尝试 2：快速快照轮询

用循环 `ps` 加 `sleep 0.3s`，大概率能抓到瞬时 adb 进程：

```bash
for i in $(seq 1 30); do
  ps -o pid,ppid,command -p $(pgrep -x adb) 2>/dev/null \
    | grep -v "fork-server\|logcat\|scrcpy-server\|PID"
  sleep 0.3
done | sort -u
```

抓到了：

```
45800 42272 adb shell am start -n com.deviceagent/.MainActivity
```

**`PPID=42272`** 就是父进程 —— 真正循环的那个。

> 辅助佐证：`netstat -an | grep 5037 | grep TIME_WAIT` 会看到大量到 adb server 5037 端口的 TIME_WAIT 连接，说明确实有"频繁短连接"，和"持续 ESTABLISHED"不同。

### 尝试 3：追父进程

```bash
ps -o pid,ppid,user,command -p 42272
```

输出（格式化后）：

```
PID=42272  PPID=1  USER=qf
COMMAND=/bin/zsh -c
  source ~/.zshrc 2>/dev/null
  adb shell "log" 2>/dev/null | head -5 || echo "no log command"
  adb shell "am force-stop com.deviceagent"
  sleep 1
  adb logcat -c 2>/dev/null
  sleep 1
  # Start activity
  (while true; do
     adb shell "am start -n com.deviceagent/.MainActivity" 2>/dev/null
     sleep 10
   done) &
  sleep 3
  adb logcat -d 2>&1 | grep "com.deviceagent" | head -20
```

**水落石出**：

- 这是一段 `zsh -c` 包裹的调试脚本（多半是某次 AI/自动化 session 跑过的）。
- 中间 `(while true; do ...; sleep 10; done) &` 把循环 fork 进了**后台子 shell**。
- 父 `zsh -c` 执行完 `head -20` 后退出，但后台子 shell 的父进程被 `launchd`（pid 1）收养，变成**孤儿进程**继续跑。
- 结果就是：这个脚本每 10 秒稳定地 `am start` 一次，直到被显式 kill。

## 第四步：修复

```bash
kill 42272
pkill -f "am start -n com.deviceagent/.MainActivity"
```

验证：

```bash
adb logcat -c
sleep 25
adb logcat -d 2>&1 | grep -cE "ActivityTaskManager.*START.*deviceagent"
# -> 0
```

清零，问题解决。

## 复盘：这次学到什么

### 诊断思路

1. **代码看起来"不可能"被触发时，先确认是被外部显式调用，而不是代码自己拉起来的**。`callingPackage` / `uid` / `from pid` 是关键。
2. **`callingPackage=com.android.shell` + `uid=2000` 几乎 100% 是 `adb shell` 或设备上以 shell uid 运行的进程（如 scrcpy server）** 触发的。
3. **时间间隔是很强的线索**。本例恰好 10 秒间隔，既匹配代码里 `CommandPoller` 的 POLL_MS，也容易让人误判。看 `CommandPoller` 日志和 `ActivityTaskManager` 日志的**偏移是否固定**，就能判断它们是否相关：本例两者偏移 ~4 秒且独立，说明不是同一个调度。

### 工具箱（这次用到的）

| 目的 | 命令 |
|---|---|
| 看谁在拉起 Activity | `adb logcat -d \| grep "ActivityTaskManager.*START"` |
| 按 app pid 过滤日志 | `adb shell 'logcat -d --pid=$(pidof com.deviceagent)'` |
| 列出所有 adb 客户端 | `lsof -i :5037 -sTCP:ESTABLISHED` |
| 看 adb 连接有无 TIME_WAIT 风暴 | `netstat -an \| grep 5037 \| grep TIME_WAIT \| wc -l` |
| 抓瞬时短命进程 | `for i in $(seq 1 30); do ps ... ; sleep 0.3; done \| sort -u` |
| 查父进程链 | `ps -o pid,ppid,user,command -p <pid>` |
| 设备端进程 | `adb shell ps -A` |

### 容易踩的坑

- **把 `callingActivity=null` 打成 "system" 会误导**。更靠谱的做法是日志里打 `intent.component` / `intent.flags` / `referrer`（`getReferrer()` 能拿到 calling package 的 URI，哪怕 callingActivity 是 null）。
- **`ps -ef` 抓不到一次性命令**。必须高频轮询或用 `dtrace`/`fs_usage`（macOS）/`execsnoop`（Linux）这种基于内核事件的工具。
- **后台 `&` 的孤儿进程**不会随原终端关闭而死。以后写这种调试脚本时，习惯性带 `trap 'kill 0' EXIT` 之类的兜底，或者用 `timeout 300 bash -c '...'` 强制上限。

### 对这个工程本身的建议（和本次 bug 无关，顺手记下）

- `InstallTestActivity` 声明了 `action.MAIN` 但没 `category.LAUNCHER`，整个 app 没有桌面入口。如果要调试入口，加 `category.LAUNCHER`；如果不需要，移掉那个 `intent-filter` 免得误导。
- `MainActivity` 既然只是"透明跳板 → 拉 Service"，可以考虑直接删掉，让 `BootReceiver` 继续承担唯一的启动职责。现在 MainActivity 的存在反而给外部"误触发"留了口子。
