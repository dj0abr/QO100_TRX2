#include "pulse_usage.hpp"
#include <cstring>
#include <stdexcept>
#include <sys/time.h>
#include <unistd.h>

namespace {
void release(pa_operation* op) {if(op)pa_operation_unref(op);}
bool own(const pa_proplist* properties) {
    if(!properties)return false;
    const char* id=pa_proplist_gets(properties,PA_PROP_APPLICATION_PROCESS_ID);
    return id && std::to_string(getpid())==id;
}
timeval later() {timeval t{};gettimeofday(&t,nullptr);t.tv_sec+=2;return t;}
}
PulseUsage::PulseUsage(std::string from,std::string to,std::string internal,std::function<void()> changed)
    :from_(std::move(from)),to_(std::move(to)),internal_(std::move(internal)),changed_(std::move(changed)) {
    loop_=pa_threaded_mainloop_new();
    if(!loop_)throw std::runtime_error("Cannot create PulseAudio device watcher");
    context_=pa_context_new(pa_threaded_mainloop_get_api(loop_),"QO100 device usage");
    if(!context_) {pa_threaded_mainloop_free(loop_);throw std::runtime_error("Cannot create PulseAudio watcher context");}
    pa_context_set_state_callback(context_,connection,this);
    if(pa_context_connect(context_,nullptr,PA_CONTEXT_NOAUTOSPAWN,nullptr)<0 || pa_threaded_mainloop_start(loop_)<0) {
        pa_context_set_state_callback(context_,nullptr,nullptr);pa_context_unref(context_);pa_threaded_mainloop_free(loop_);
        throw std::runtime_error("Cannot connect PulseAudio device watcher");
    }
}
PulseUsage::~PulseUsage() {
    pa_threaded_mainloop_stop(loop_);
    if(timer_)pa_threaded_mainloop_get_api(loop_)->time_free(timer_);
    pa_context_set_state_callback(context_,nullptr,nullptr);pa_context_set_subscribe_callback(context_,nullptr,nullptr);
    pa_context_disconnect(context_);pa_context_unref(context_);pa_threaded_mainloop_free(loop_);
}
PulseUsage::State PulseUsage::state() const {
    const auto flags=state_.load(std::memory_order_acquire);
    return {bool(flags&1),bool(flags&2),bool(flags&4),flags!=0};
}
void PulseUsage::publish(unsigned value) {
    if(state_.exchange(value,std::memory_order_acq_rel)!=value)changed_();
}
void PulseUsage::connection(pa_context* c,void* data) {
    auto& self=*static_cast<PulseUsage*>(data);
    const auto state=pa_context_get_state(c);
    if(state==PA_CONTEXT_READY) {
        pa_context_set_subscribe_callback(c,event,&self);
        release(pa_context_subscribe(c,pa_subscription_mask_t(PA_SUBSCRIPTION_MASK_SINK_INPUT|
            PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT|PA_SUBSCRIPTION_MASK_SINK|PA_SUBSCRIPTION_MASK_SOURCE),nullptr,nullptr));
        if(!self.timer_) {
            const auto t=later();auto* api=pa_threaded_mainloop_get_api(self.loop_);
            self.timer_=api->time_new(api,&t,timer,&self);
        }
        self.refresh();
    } else if(state==PA_CONTEXT_FAILED || state==PA_CONTEXT_TERMINATED)self.publish(3); // Detection failure: preserve legacy audio.
}
void PulseUsage::event(pa_context*,pa_subscription_event_type_t,uint32_t,void* data) {
    static_cast<PulseUsage*>(data)->refresh();
}
void PulseUsage::timer(pa_mainloop_api* api,pa_time_event* event,const timeval*,void* data) {
    auto& self=*static_cast<PulseUsage*>(data);
    if(pa_context_get_state(self.context_)==PA_CONTEXT_READY)self.refresh();
    const auto t=later();api->time_restart(event,&t);
}
void PulseUsage::refresh() {
    if(busy_) {dirty_=true;return;}
    busy_=true;dirty_=failed_=rx_=tx_=false;pending_=3;
    sink_=from_index_=monitor_=remap_=PA_INVALID_INDEX;
    auto* op=pa_context_get_sink_info_by_name(context_,to_.c_str(),sink,this);
    if(op)release(op);else devices_done(-1);
    op=pa_context_get_source_info_by_name(context_,from_.c_str(),source,this);
    if(op)release(op);else devices_done(-1);
    op=pa_context_get_source_info_by_name(context_,(internal_+".monitor").c_str(),source,this);
    if(op)release(op);else devices_done(-1);
}
void PulseUsage::sink(pa_context*,const pa_sink_info* info,int end,void* data) {
    auto& self=*static_cast<PulseUsage*>(data);
    if(info)self.sink_=info->index;
    if(end)self.devices_done(end<0);
}
void PulseUsage::source(pa_context*,const pa_source_info* info,int end,void* data) {
    auto& self=*static_cast<PulseUsage*>(data);
    if(info) {
        if(self.from_==info->name) {self.from_index_=info->index;self.remap_=info->owner_module;}
        else self.monitor_=info->index;
    }
    if(end)self.devices_done(end<0);
}
void PulseUsage::devices_done(int error) {
    failed_=failed_ || error!=0;
    if(--pending_)return;
    pending_=2;
    auto* op=pa_context_get_sink_input_info_list(context_,inputs,this);
    if(op)release(op);else streams_done(-1);
    op=pa_context_get_source_output_info_list(context_,outputs,this);
    if(op)release(op);else streams_done(-1);
}
void PulseUsage::inputs(pa_context*,const pa_sink_input_info* info,int end,void* data) {
    auto& self=*static_cast<PulseUsage*>(data);
    if(info && !info->corked && info->sink==self.sink_ && !own(info->proplist))self.tx_=true;
    if(end)self.streams_done(end<0);
}
void PulseUsage::outputs(pa_context*,const pa_source_output_info* info,int end,void* data) {
    auto& self=*static_cast<PulseUsage*>(data);
    if(info && !info->corked && !own(info->proplist)) {
        const bool direct=info->source==self.from_index_;
        const bool internal=info->source==self.monitor_ && info->client!=PA_INVALID_INDEX && info->owner_module!=self.remap_;
        if(direct || internal)self.rx_=true;
    }
    if(end)self.streams_done(end<0);
}
void PulseUsage::streams_done(int error) {
    failed_=failed_ || error!=0;
    if(--pending_)return;
    const bool known=!failed_ && sink_!=PA_INVALID_INDEX && from_index_!=PA_INVALID_INDEX;
    publish(known?(4|(rx_?1:0)|(tx_?2:0)):3);
    busy_=false;
    if(dirty_)refresh();
}
