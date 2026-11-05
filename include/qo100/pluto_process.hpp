#pragma once
#include "qo100/config.hpp"
#include <filesystem>
#include <sys/types.h>

namespace qo100 {
class PlutoProcess {
public:
    PlutoProcess(const Config& config, const std::filesystem::path& executable);
    ~PlutoProcess();
    PlutoProcess(const PlutoProcess&) = delete;
    PlutoProcess& operator=(const PlutoProcess&) = delete;
    void check();
    const std::filesystem::path& log_path() const { return log_; }
private:
    pid_t pid_ = -1;
    std::filesystem::path directory_, log_;
};
}
