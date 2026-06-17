#ifdef _WIN32

#include "executor/executor.h"

#include "logger/logger.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace device_agent {

std::string WindowsExecutor::reboot(bool force, const std::string& command_id, std::string& err) {
    (void)force;
    LOG_INFO("WindowsExecutor: scheduling reboot, command_id=" + command_id);
    if (std::getenv("DEVICE_AGENT_TEST_MODE") != nullptr) {
        LOG_WARN("WindowsExecutor: TEST MODE - skipping real reboot");
        return "pending";
    }

    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        ::ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_APPLICATION);
    }).detach();
    return "pending";
}

void WindowsExecutor::updateConfig(const std::string& key, const std::string& value, std::string& err) {
    (void)err;
    LOG_INFO("WindowsExecutor: updateConfig key=" + key + " value=" + value);
    const char* temp = std::getenv("TEMP");
    std::string path = temp && temp[0] ? std::string(temp) + "\\device-agent-config.bak"
                                      : "device-agent-config.bak";
    std::ofstream ofs(path);
    if (ofs.is_open()) {
        ofs << "# device-agent config\n" << key << " = " << value << "\n";
    }
}

void WindowsExecutor::upgradeFirmware(const std::string& url, const std::string& md5, std::string& err) {
    (void)md5;
    LOG_INFO("WindowsExecutor: upgradeFirmware url=" + url);
    err = "upgradeFirmware is not implemented on Windows";
}

void WindowsExecutor::upgradeApp(const std::string& appPath,
                                 const std::string& md5,
                                 const std::string& command_id,
                                 std::string& err) {
    (void)appPath;
    (void)md5;
    (void)command_id;
    err = "upgradeApp is only supported on Android";
}

}  // namespace device_agent

#endif  // _WIN32
