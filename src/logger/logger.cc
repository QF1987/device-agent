#include "logger/logger.h"
#include <cstdio>
#include <cstring>

namespace device_agent {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

Logger::Logger() : file_(nullptr) {}

Logger::~Logger() {
    if (file_) {
        fclose(file_);
    }
}

void Logger::set_level(LogLevel level) {
    std::lock_guard<std::mutex> lock(mu_);
    min_level_ = level;
}

void Logger::set_output(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    if (!filepath.empty()) {
        file_ = fopen(filepath.c_str(), "a");
    }
}

void Logger::log(LogLevel level, const std::string& tag, const std::string& msg) {
    if (level < min_level_) return;

    std::lock_guard<std::mutex> lock(mu_);

    std::ostringstream oss;
    oss << "[" << now_str() << "] "
        << "[" << level_str(level) << "] "
        << "[" << tag << "] "
        << msg << "\n";

    std::string line = oss.str();

    // stdout
    std::cout << line;
    fflush(stdout);

    // file
    if (file_) {
        fputs(line.c_str(), file_);
        fflush(file_);
    }
}

void Logger::debug(const std::string& msg) {
    log(LogLevel::DEBUG, "DA", msg);
}

void Logger::info(const std::string& msg) {
    log(LogLevel::INFO, "DA", msg);
}

void Logger::warn(const std::string& msg) {
    log(LogLevel::WARN, "DA", msg);
}

void Logger::error(const std::string& msg) {
    log(LogLevel::ERROR, "DA", msg);
}

std::string Logger::level_str(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO:  return "INFO ";
        case LogLevel::WARN:  return "WARN ";
        case LogLevel::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

std::string Logger::now_str() {
    char buf[64];
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
    return std::string(buf);
}

}  // namespace device_agent
