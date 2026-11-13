#pragma once
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace bridge {
struct FtQsoRecord {
    std::string call, own_call, grid, own_grid, mode, sent_report, received_report;
    std::time_t start, end;
    uint64_t tx_hz, rx_hz;
};

class FtLogbook {
public:
    explicit FtLogbook(std::filesystem::path path);
    bool worked_recently(const std::string& call, std::time_t now) const;
    void save(const FtQsoRecord& qso);

private:
    using Key = std::tuple<std::string, std::string, std::string, std::time_t>;
    std::filesystem::path path_;
    std::map<std::string, std::time_t> latest_;
    std::set<Key> records_;
};
}
