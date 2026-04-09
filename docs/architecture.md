# Device-Agent 架构设计

## 核心理念

device-agent 是**纯设备管理层**，只关心：
- 心跳和存活
- 系统指标（CPU/内存/磁盘/网络）
- 服务端指令执行
- 事件上报

**业务数据不写入 device-agent**，而是通过接口注入。

## 模块分层

```
┌─────────────────────────────────────────────────┐
│              device-agent (本项目)                │
├─────────────────────────────────────────────────┤
│  DeviceClient    │ gRPC → DeviceOps Server       │
│  CommandHandler  │ 处理指令                      │
│  MetricsCollector│ 系统指标采集                  │
├─────────────────┴───────────────────────────────┤
│           扩展接口层 (IBusinessBridge)           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐     │
│  │SocketBridge│  │HttpBridge│  │PluginBridge│   │
│  └──────────┘  └──────────┘  └──────────┘     │
├─────────────────────────────────────────────────┤
│  BusinessApp  (业务应用，实现 IBusinessHandler)   │
│  • 自助购药机  • 充电桩  • 机器人  • 通用设备   │
└─────────────────────────────────────────────────┘
```

## 扩展接口

### IBusinessHandler (业务应用实现)

```cpp
// 业务应用实现此接口，注册到 Bridge
class IBusinessHandler {
public:
    virtual ~IBusinessHandler() = default;

    // 获取业务指标（device-agent 定期调用）
    // 返回 JSON 字符串，如：
    // {"transactions_today": 42, "inventory_alert": false, "custom_field": "..."}
    virtual std::string get_business_metrics() = 0;

    // 获取业务状态（设备状态的一部分）
    // 返回 JSON 字符串，如：
    // {"printer_status": "ok", "medicine_slots": 20}
    virtual std::string get_business_status() = 0;

    // 执行业务指令（CommandService 下发的自定义指令）
    // command_type: "custom" 或自定义类型
    // payload_json: 指令参数
    // 返回：是否成功，error_message 填失败原因
    virtual bool execute_business_command(
        const std::string& command_type,
        const std::string& payload_json,
        std::string& error_message) = 0;

    // 业务启动完成回调（device-agent 初始化后调用）
    virtual void on_ready() = 0;
};
```

### Bridge 接口

```cpp
// 传输层 Bridge 接口（Socket/HTTP/Plugin 实现）
class IBridge {
public:
    virtual ~IBridge() = default;

    // 启动 Bridge（监听端口/文件描述符等）
    virtual bool start() = 0;

    // 停止 Bridge
    virtual void stop() = 0;

    // 注册业务处理器
    virtual void set_handler(std::shared_ptr<IBusinessHandler> handler) = 0;

    // Bridge 类型标识
    virtual const char* type() const = 0;  // "socket" / "http" / "plugin"
};
```

## Socket Bridge 设计

### Unix Domain Socket (Linux/Mac) / TCP (Windows)

- 文件路径/端口：配置项指定
- 协议：简单 JSON 行协议（每条消息一行 JSON）
- 模式：业务应用主动连接，保持长连接

### 消息格式

**业务应用 → device-agent：**

```json
// 上报业务指标
{"type": "metrics", "data": {"transactions_today": 42}}
```

```json
// 上报业务状态
{"type": "status", "data": {"printer_status": "ok"}}
```

```json
// 指令执行结果
{"type": "command_result", "id": "uuid", "success": true, "message": "ok"}
```

**device-agent → 业务应用：**

```json
// 请求执行业务指令
{"type": "execute_command", "id": "uuid", "command_type": "custom", "payload": "{}"}
```

### 心跳保活

- device-agent 每 5 秒向 socket 发 ping
- 业务应用 15 秒内无响应 → 断开重连
- 业务应用主动断开 → agent 继续运行，等待重连

## HTTP Bridge 设计

- 监听 localhost:端口
- POST /metrics — 上报业务指标
- POST /status — 上报业务状态
- POST /command/result — 回报指令执行结果
- GET /health — 健康检查（给 systemd/launchd 用）

## Plugin Bridge 设计

- 编译成 .so (Linux) / .dylib (Mac) / .dll (Windows)
- device-agent dlopen 加载
- 入口：`create_bridge()` 返回 `IBridge*`
- 业务逻辑内嵌在 plugin 里

## 配置扩展

```json
{
  "device_id": "SH-PD-001",
  "token": "...",
  "server_host": "...",
  "server_port": 9090,

  "business_bridge": {
    "type": "socket",
    "path": "/var/run/device-agent/business.sock",
    "reconnect_interval": 5
  }
}
```

## 实现顺序

1. ✅ **已完成** — device-agent 核心（gRPC client、CommandHandler、Logger）
2. **当前** — Socket Bridge + IBusinessHandler 接口
3. 下一阶段 — HTTP Bridge
4. 后续 — Plugin Bridge
