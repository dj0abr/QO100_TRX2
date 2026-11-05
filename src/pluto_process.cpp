#include "qo100/pluto_process.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace qo100 {
PlutoProcess::PlutoProcess(const Config& c, const std::filesystem::path& binary) {
    const auto exe = std::filesystem::canonical(binary);
    directory_ = exe.parent_path().parent_path() / "run" / ("pluto-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory_);
    log_ = directory_ / "pluto.log";
    std::ofstream config(directory_ / "pluto_config.txt");
    config << std::fixed << std::setprecision(6)
           << "UDP_IPADDRESS 127.0.0.1\nPLUTO_ID " << c.pluto_uri
           << "\nRX_FREQ " << c.pluto_rx_hz / 1e6
           << "\nTX_FREQ " << c.pluto_tx_hz / 1e6
           << "\nSAMPLERATE " << c.pluto_sample_rate / 1e6
           << "\nRX_BANDWIDTH " << c.rx_bandwidth_hz / 1e6
           << "\nTX_BANDWIDTH 0.2\nTX_GAIN -79\nCROSSBANDREPEATER 0\nRX_ONLY 1\n";
    config.close();
    if (!config) throw std::runtime_error("Cannot write Pluto configuration");
    const int logfd = open(log_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (logfd < 0) throw std::runtime_error("Cannot open Pluto log");
    int errors[2];
    if (pipe2(errors, O_CLOEXEC) < 0) { close(logfd); throw std::runtime_error("Cannot create child startup pipe"); }
    const pid_t parent = getpid();
    pid_ = fork();
    if (pid_ < 0) {
        close(logfd); close(errors[0]); close(errors[1]);
        throw std::runtime_error("Cannot fork Pluto driver");
    }
    if (pid_ == 0) {
        close(errors[0]);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(125);
        signal(SIGINT, SIG_DFL); signal(SIGTERM, SIG_DFL);
        if (chdir(directory_.c_str()) < 0 || dup2(logfd, STDOUT_FILENO) < 0 || dup2(logfd, STDERR_FILENO) < 0) {
            const int error = errno;
            (void)!write(errors[1], &error, sizeof(error));
            _exit(126);
        }
        close(logfd);
        execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        const int error = errno;
        (void)!write(errors[1], &error, sizeof(error));
        _exit(127);
    }
    close(logfd); close(errors[1]);
    int error = 0;
    ssize_t n;
    do { n = read(errors[0], &error, sizeof(error)); } while (n < 0 && errno == EINTR);
    close(errors[0]);
    if (n != 0) {
        kill(pid_, SIGTERM);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
        throw std::runtime_error("Cannot start Pluto driver: " + std::string(std::strerror(error ? error : EIO)));
    }
}
PlutoProcess::~PlutoProcess() {
    if (pid_ <= 0) return;
    kill(pid_, SIGTERM);
    for (int i = 0; i < 40; ++i) {
        const auto result = waitpid(pid_, nullptr, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    kill(pid_, SIGKILL);
    while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
}
void PlutoProcess::check() {
    int status = 0;
    const auto result = waitpid(pid_, &status, WNOHANG);
    if (result == pid_) {
        pid_ = -1;
        throw std::runtime_error("Pluto driver exited (wait status " + std::to_string(status) + "); see " + log_.string());
    }
    if (result < 0 && errno != EINTR) throw std::runtime_error("Cannot monitor Pluto driver");
}
}
