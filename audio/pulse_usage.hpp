#pragma once
#include <pulse/pulseaudio.h>
#include <atomic>
#include <functional>
#include <string>

// Introspection and subscriptions use one persistent libpulse connection.
// No pactl polling processes or audio-level heuristics in normal operation.
class PulseUsage {
public:
    struct State {bool rx=false,tx=false,known=false,available=false;};
    PulseUsage(std::string from,std::string to,std::string internal,std::function<void()> changed);
    ~PulseUsage();
    State state() const;
private:
    void refresh();void devices_done(int error);void streams_done(int error);void publish(unsigned value);
    static void connection(pa_context*,void*);
    static void event(pa_context*,pa_subscription_event_type_t,uint32_t,void*);
    static void timer(pa_mainloop_api*,pa_time_event*,const timeval*,void*);
    static void sink(pa_context*,const pa_sink_info*,int,void*);
    static void source(pa_context*,const pa_source_info*,int,void*);
    static void inputs(pa_context*,const pa_sink_input_info*,int,void*);
    static void outputs(pa_context*,const pa_source_output_info*,int,void*);
    pa_threaded_mainloop* loop_=nullptr;pa_context* context_=nullptr;pa_time_event* timer_=nullptr;
    std::string from_,to_,internal_;std::function<void()> changed_;
    std::atomic<unsigned> state_{0};
    uint32_t sink_=PA_INVALID_INDEX,from_index_=PA_INVALID_INDEX,monitor_=PA_INVALID_INDEX,remap_=PA_INVALID_INDEX;
    unsigned pending_=0;bool busy_=false,dirty_=false,failed_=false,rx_=false,tx_=false;
};
