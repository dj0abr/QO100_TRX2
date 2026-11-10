#include "qo100/dsp_workers.hpp"
#include <pthread.h>
#include <stdexcept>
#include <utility>
#include <string>

namespace qo100 {
DspWorker::DspWorker(const char* name) : thread_([this,name=std::string(name)] { run(name.c_str()); }) {}
DspWorker::~DspWorker() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    work_.notify_one();
    thread_.join(); // Finish even an unwaited job before referenced DSP is freed.
}
void DspWorker::start(std::function<void()> job) {
    if (!job) throw std::invalid_argument("Empty DSP job");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) throw std::logic_error("DSP job must be joined before reuse");
        job_ = std::move(job); pending_ = true; completed_ = false;
    }
    work_.notify_one();
}
void DspWorker::wait() {
    std::exception_ptr error;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock,[&] { return !pending_ || completed_; });
        pending_ = false; error = std::exchange(error_,{});
    }
    if (error) std::rethrow_exception(error);
}
void DspWorker::run(const char* name) {
    pthread_setname_np(pthread_self(),name);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_.wait(lock,[&] { return stopping_ || bool(job_); });
            if (!job_) return;
            job = std::exchange(job_,{});
        }
        std::exception_ptr error;
        try { job(); } catch (...) { error = std::current_exception(); }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = error; completed_ = true;
        }
        done_.notify_one();
    }
}
namespace {
constexpr std::array<size_t,4> rx_group_edges{0,1,3,5};
constexpr std::array<const char*,3> rx_group_names{"qo100-rx1","qo100-rx2-3","qo100-rx4-5"};
}
RxWorkers::RxWorkers(std::array<RxChannel*,5> channels) : channels_(channels) {
    for(size_t group=0;group<workers_.size();++group) {
        bool active=false;
        for(size_t i=rx_group_edges[group];i<rx_group_edges[group+1];++i)
            active=active || channels_[i]!=nullptr;
        if(active) workers_[group]=std::make_unique<DspWorker>(rx_group_names[group]);
    }
}
void RxWorkers::process(const std::complex<float>* samples, size_t count,
                        const std::function<void()>& main_work,uint32_t enabled_mask) {
    std::array<bool,3> started{};
    std::exception_ptr error;
    try {
        for (size_t group = 0; group < workers_.size(); ++group) {
            bool active=false;
            for(size_t i=rx_group_edges[group];i<rx_group_edges[group+1];++i)
                active=active || (channels_[i] && (enabled_mask&(1u<<i)));
            if(!workers_[group] || !active) continue;
            workers_[group]->start([this,samples,count,group,enabled_mask] {
                for (size_t i = rx_group_edges[group]; i < rx_group_edges[group+1]; ++i)
                    if(channels_[i] && (enabled_mask&(1u<<i))) channels_[i]->push_block(samples,count);
            });
            started[group]=true;
        }
        main_work();
    } catch (...) { error = std::current_exception(); }
    for (size_t i = 0; i < workers_.size(); ++i) {
        if(!started[i]) continue;
        try { workers_[i]->wait(); }
        catch (...) { if (!error) error = std::current_exception(); }
    }
    if (error) std::rethrow_exception(error);
}
}
