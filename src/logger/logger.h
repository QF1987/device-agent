#pragma once

#include <string>
#include <ctime>
#include <iostream>
#include <sstream>
#include <mutex>

namespace device_agent {

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level);
    void set_output(const std::string& filepath);

    void debug(const std::string& msg);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);

    // Thread-safe logging with formatted message
    void log(LogLevel level, const std::string& tag, const std::string& msg);

private:
    Logger();
    ~Logger();

    std::string level_str(LogLevel level);
    std::string now_str();

    LogLevel min_level_ = LogLevel::INFO;
    FILE* file_ = nullptr;
    std::mutex mu_;
};

#define LOG_DEBUG(msg) device_agent::Logger::instance().debug(msg)
#define LOG_INFO(msg)  device_agent::Logger::instance().info(msg)
#define LOG_WARN(msg)  device_agent::Logger::instance().warn(msg)
#define LOG_ERROR(msg) device_agent::Logger::instance().error(msg)

}  // namespace device_agent
