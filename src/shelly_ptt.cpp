#include "shelly_ptt.hpp"

#include <curl/curl.h>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace qo100 {
namespace {
using namespace std::chrono_literals;
constexpr auto retry = 250ms;
constexpr auto refresh = 1s;
constexpr auto lease = 4s;

size_t discard(char*, size_t size, size_t count, void*) { return size * count; }

bool switch_relay(CURL* http, const std::string& ip, bool on) {
    const auto url = "http://" + ip + "/rpc/Switch.Set?id=0&on=" +
        (on ? "true&toggle_after=" + std::to_string(lease.count()) : "false");
    curl_easy_setopt(http, CURLOPT_URL, url.c_str());
    const auto result = curl_easy_perform(http);
    long status = 0;
    curl_easy_getinfo(http, CURLINFO_RESPONSE_CODE, &status);
    if (result == CURLE_OK && status == 200) return true;
    std::cerr << "Shelly PTT " << ip << (on ? " on: " : " off: ")
              << (result == CURLE_OK ? std::to_string(status) : curl_easy_strerror(result)) << '\n';
    return false;
}
}

ShellyPtt::ShellyPtt(const std::string& ip) : ip_(ip) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        throw std::runtime_error("Cannot initialize Shelly HTTP client");
    try {
        worker_ = std::thread(&ShellyPtt::run, this);
    } catch (...) {
        curl_global_cleanup();
        throw;
    }
}

ShellyPtt::~ShellyPtt() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    worker_.join();
    curl_global_cleanup();
}

void ShellyPtt::configure(const std::string& ip) {
    std::lock_guard lock(mutex_);
    if (ip_ == ip) return;
    ++revision_;
    ip_ = ip;
    on_since_ = valid_until_ = {};
    requested_ = false;
    wake_.notify_one();
}

void ShellyPtt::request(bool on) {
    std::lock_guard lock(mutex_);
    if (requested_ == on) return;
    ++revision_;
    requested_ = on;
    if (!on) on_since_ = valid_until_ = {};
    wake_.notify_one();
}

bool ShellyPtt::ready(Clock::time_point now, std::chrono::milliseconds lead) const {
    std::lock_guard lock(mutex_);
    return ip_.empty() || (requested_ && now < valid_until_ && now >= on_since_ + lead);
}

void ShellyPtt::run() {
    auto* http = curl_easy_init();
    if (!http) {
        std::cerr << "Cannot create Shelly HTTP client\n";
        return;
    }
    curl_easy_setopt(http, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(http, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(http, CURLOPT_TIMEOUT_MS, 250L);
    curl_easy_setopt(http, CURLOPT_WRITEFUNCTION, discard);

    std::unique_lock lock(mutex_);
    std::string ip;
    uint64_t completed = 0;
    auto next = Clock::time_point::max();
    while (!stopping_) {
        if (ip != ip_) {
            auto old_ip = std::exchange(ip, ip_);
            lock.unlock();
            if (!old_ip.empty()) switch_relay(http, old_ip, false);
            lock.lock();
            next = Clock::now(); // Synchronize the new relay at startup and after setup changes.
        } else if (!ip.empty() && (completed != revision_ || Clock::now() >= next)) {
            const bool on = requested_;
            completed = revision_;
            lock.unlock();
            const auto sent = Clock::now();
            const bool ok = switch_relay(http, ip, on);
            const auto now = Clock::now();
            lock.lock();
            if (completed == revision_ && on && ok) {
                if (sent >= valid_until_ || now >= valid_until_) on_since_ = now;
                // The device starts its timer after the request leaves this process.
                valid_until_ = sent + lease;
            }
            if (!ok) next = now + retry;
            else next = on ? now + refresh : Clock::time_point::max();
        } else if (ip.empty()) {
            wake_.wait(lock);
        } else {
            wake_.wait_until(lock, next);
        }
    }
    const auto configured_ip = ip_;
    lock.unlock();
    if (!ip.empty()) switch_relay(http, ip, false);
    if (!configured_ip.empty() && configured_ip != ip) switch_relay(http, configured_ip, false);
    curl_easy_cleanup(http);
}
}
