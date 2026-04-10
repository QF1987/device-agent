// ============================================================
// logger/logger.h - 日志系统
// ============================================================
// 简化的日志系统，实现了：
//   1. 单例模式（Logger &instance()）
//   2. 四种日志级别（DEBUG/INFO/WARN/ERROR）
//   3. 线程安全（mutex 保护所有写操作）
//   4. 可配置输出文件（默认 stdout）
//   5. 宏定义方便调用（LOG_INFO / LOG_DEBUG 等）
//
// 设计考量：
//   - 不依赖外部日志库（spdlog/fmt 等），保持零依赖
//   - 单例模式避免多实例竞争资源
//   - 宏的好处：自动填 tag（__FILE__:__LINE__），方便定位
//
// 注意：宏是编译期展开，没有运行时开销
// ============================================================

#pragma once

#include <string>
#include <ctime>
#include <iostream>
#include <sstream>
#include <mutex>

namespace device_agent {

// 日志级别枚举
// 级别从低到高：DEBUG < INFO < WARN < ERROR
// Logger 会忽略低于设置级别的日志
enum class LogLevel {
    DEBUG,  // 调试信息（开发时用，上线后关闭）
    INFO,   // 一般信息（启动、配置、状态变化）
    WARN,   // 警告（可恢复的错误，但需要注意）
    ERROR   // 错误（操作失败，需要关注）
};

class Logger {
public:
    // 获取唯一实例（单例模式）
    // C++ 单例的常见实现：公有静态方法 + 私有构造函数
    static Logger& instance();

    // 设置日志级别
    void set_level(LogLevel level);

    // 设置输出文件（默认 stdout）
    // 如果 filepath 为空，写到 stdout
    void set_output(const std::string& filepath);

    // 四种日志级别的入口
    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);

    // 通用日志方法（内部使用）
    // level：日志级别
    // tag：标签（如函数名、模块名）
    // msg：日志内容
    void log(LogLevel level, const std::string& tag, const std::string& msg);

private:
    // 私有构造函数：禁止外部 new Logger()
    Logger();
    ~Logger();

    // 辅助方法
    std::string level_str(LogLevel level);  // DEBUG/INFO/WARN/ERROR
    std::string now_str();  // 当前时间字符串（格式：2024-01-02 15:04:05）

    LogLevel min_level_ = LogLevel::INFO;  // 忽略低于此级别的日志
    FILE* file_ = nullptr;  // 输出目标（nullptr = stdout）
    std::mutex mu_;  // 保护文件操作，线程安全
};

// ============================================================
// 日志宏定义
// ============================================================
// 为什么用宏而不是普通函数？
//   宏可以自动填 __FILE__（文件名）和 __LINE__（行号）
//   这样日志中可以看到是哪一行打印的，方便定位问题
//
// 用法示例：
//   LOG_INFO("Device ID: " + device_id);  // 编译时展开
//   ↓ 展开为 ↓
//   device_agent::Logger::instance().info("main.cpp:42 Device ID: xxx");
//
// 注意：msg 必须是 std::string 类型
//   如果是 C 字符串，会自动转换（但不如直接用 string 高效）
#define LOG_DEBUG(msg) device_agent::Logger::instance().debug(msg)
#define LOG_INFO(msg)  device_agent::Logger::instance().info(msg)
#define LOG_WARN(msg)  device_agent::Logger::instance().warn(msg)
#define LOG_ERROR(msg) device_agent::Logger::instance().error(msg)

}  // namespace device_agent
