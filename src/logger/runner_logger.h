#pragma once

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace device_agent {

class RunnerLogger {
public:
    enum class Format {
        Text,
        Json,
    };

    RunnerLogger(Format format, std::string runner_id, std::ostream& out);

    void log(const std::string& event,
             const std::vector<std::pair<std::string, std::string>>& payload = {});

private:
    std::string timestamp() const;
    std::string escape_json(const std::string& value) const;

    Format format_;
    std::string runner_id_;
    std::ostream& out_;
};

}  // namespace device_agent
