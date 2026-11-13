#include "ft_logbook.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>

namespace bridge {
namespace {
std::string upper(std::string_view value) {
    std::string result(value);
    for (auto& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}

std::string utc(std::time_t value, const char* format) {
    std::tm parts{};
    char result[16];
    if (!gmtime_r(&value, &parts) || !std::strftime(result, sizeof result, format, &parts))
        throw std::runtime_error("Cannot format QSO time");
    return result;
}

std::time_t timestamp(const std::string& date, std::string time) {
    if (time.size() == 4) time += "00";
    if (date.size() != 8 || time.size() != 6) return 0;
    const auto text = date + time;
    if (text.find_first_not_of("0123456789") != std::string::npos) return 0;
    std::tm parts{};
    parts.tm_year = std::stoi(date.substr(0, 4)) - 1900;
    parts.tm_mon = std::stoi(date.substr(4, 2)) - 1;
    parts.tm_mday = std::stoi(date.substr(6, 2));
    parts.tm_hour = std::stoi(time.substr(0, 2));
    parts.tm_min = std::stoi(time.substr(2, 2));
    parts.tm_sec = std::stoi(time.substr(4, 2));
    const auto value = timegm(&parts);
    return value > 0 && utc(value, "%Y%m%d%H%M%S") == text ? value : 0;
}

std::string field(const std::string& name, const std::string& value) {
    if (value.empty()) return {};
    return "<" + name + ":" + std::to_string(value.size()) + ">" + value + " ";
}

std::string mhz(uint64_t hz) {
    std::ostringstream value;
    value << hz / 1000000 << '.' << std::setw(6) << std::setfill('0') << hz % 1000000;
    return value.str();
}
}

FtLogbook::FtLogbook(std::filesystem::path path) : path_(std::move(path)) {
    if (!std::filesystem::exists(path_)) return;
    std::ifstream input(path_, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read FT4/8 logbook");
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) throw std::runtime_error("Cannot read FT4/8 logbook");
    const std::string contents = buffer.str();
    std::map<std::string, std::string> fields;
    size_t pos = 0;
    while ((pos = contents.find('<', pos)) != std::string::npos) {
        const auto end = contents.find('>', ++pos);
        if (end == std::string::npos) break;
        const std::string_view tag(contents.data() + pos, end - pos);
        pos = end + 1;
        const auto separator = tag.find(':');
        const auto name = upper(tag.substr(0, separator));
        if (name == "EOH") fields.clear();
        if (name == "EOR") {
            const auto call = upper(fields["CALL"]);
            const auto start = timestamp(fields["QSO_DATE"], fields["TIME_ON"]);
            const auto finish = timestamp(fields["QSO_DATE_OFF"].empty() ? fields["QSO_DATE"] : fields["QSO_DATE_OFF"],
                                          fields["TIME_OFF"]);
            if (!call.empty() && start) {
                auto& latest = latest_[call];
                latest = std::max(latest, std::max(start, finish));
                const auto mode = upper(fields["SUBMODE"].empty() ? fields["MODE"] : fields["SUBMODE"]);
                records_.emplace(call, upper(fields["STATION_CALLSIGN"]), mode, start);
            }
            fields.clear();
        }
        if (separator == std::string_view::npos) continue;
        auto length_text = tag.substr(separator + 1);
        length_text = length_text.substr(0, length_text.find(':'));
        size_t length = 0;
        const auto parsed = std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
        if (parsed.ec != std::errc{} || parsed.ptr != length_text.data() + length_text.size() ||
            length > contents.size() - pos) break;
        fields[name] = contents.substr(pos, length);
        pos += length;
    }
}

bool FtLogbook::worked_recently(const std::string& call, std::time_t now) const {
    const auto entry = latest_.find(call);
    return entry != latest_.end() && entry->second <= now && now - entry->second <= 24 * 60 * 60;
}

void FtLogbook::save(const FtQsoRecord& qso) {
    const auto key = Key(qso.call, qso.own_call, qso.mode, qso.start);
    if (records_.count(key)) return;

    const auto record = field("CALL", qso.call) + field("STATION_CALLSIGN", qso.own_call) +
        field("QSO_DATE", utc(qso.start, "%Y%m%d")) + field("TIME_ON", utc(qso.start, "%H%M%S")) +
        field("QSO_DATE_OFF", utc(qso.end, "%Y%m%d")) + field("TIME_OFF", utc(qso.end, "%H%M%S")) +
        field("MODE", qso.mode == "FT4" ? "MFSK" : "FT8") + field("SUBMODE", qso.mode == "FT4" ? "FT4" : "") +
        field("BAND", "13cm") + field("FREQ", mhz(qso.tx_hz)) +
        field("BAND_RX", "3cm") + field("FREQ_RX", mhz(qso.rx_hz)) +
        field("PROP_MODE", "SAT") + field("SAT_NAME", "QO-100") +
        field("GRIDSQUARE", qso.grid) + field("MY_GRIDSQUARE", qso.own_grid) +
        field("RST_SENT", qso.sent_report) + field("RST_RCVD", qso.received_report) + "<EOR>\n";

    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path());
    auto temporary = path_.string() + ".tmp-XXXXXX";
    const int fd = mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error("Cannot create temporary FT4/8 logbook");
    try {
        std::ofstream output(temporary, std::ios::binary);
        if (std::filesystem::exists(path_) && std::filesystem::file_size(path_) > 0) {
            std::ifstream previous(path_, std::ios::binary);
            if (!previous) throw std::runtime_error("Cannot read FT4/8 logbook");
            output << previous.rdbuf() << '\n';
            if (previous.bad()) throw std::runtime_error("Cannot read FT4/8 logbook");
        } else {
            output << "QO100_TRX2 FT4/8 Logbook\n<ADIF_VER:5>3.1.8 <PROGRAMID:10>QO100_TRX2 <EOH>\n";
        }
        output << record;
        output.close();
        if (!output || fsync(fd) != 0) throw std::runtime_error("Cannot write FT4/8 logbook");
        std::filesystem::rename(temporary, path_);
    } catch (...) {
        close(fd);
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        throw;
    }
    close(fd);
    records_.insert(key);
    auto& latest = latest_[qso.call];
    latest = std::max(latest, qso.end);
}
}
