// RX2/TX2 virtual sound interface. The backend remains clocked by the Pluto FIFO.
#include "qo100/channels.hpp"
#include "qo100/tx_channel.hpp"
#include "wire.hpp"
#include "pulse_usage.hpp"
#include <condition_variable>
#include <memory>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <spawn.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

extern char** environ;
namespace {
using Clock=std::chrono::steady_clock;
std::atomic<bool> running{true};
static_assert(std::atomic<bool>::is_always_lock_free);
void stop(int) {running=0;}
struct Fd {int fd=-1;explicit Fd(int value):fd(value) {} ~Fd(){if(fd>=0) close(fd);} Fd(const Fd&)=delete;};
std::string command(std::vector<std::string> args) {
    int pipefd[2];if(pipe2(pipefd,O_CLOEXEC)) throw std::runtime_error("Cannot create pactl output pipe");
    Fd input(pipefd[0]),output(pipefd[1]);
    posix_spawn_file_actions_t actions;posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,output.fd,STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,input.fd);
    std::vector<char*> argv;for(auto& arg:args) argv.push_back(arg.data());argv.push_back(nullptr);
    pid_t pid;const auto error=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions);
    if(error) throw std::runtime_error("Cannot start pactl (install pulseaudio-utils)");
    close(output.fd);output.fd=-1;
    std::string result;std::array<char,4096> buffer{};ssize_t n;
    while((n=read(input.fd,buffer.data(),buffer.size()))>0) result.append(buffer.data(),n);
    int status=0;while(waitpid(pid,&status,0)<0 && errno==EINTR) {}
    if(!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("pactl failed; check the user's PulseAudio/PipeWire connection");
    return result;
}
struct Devices {
    std::vector<std::string> modules;
    void ensure(const std::string& kind,const std::string& name,std::vector<std::string> args) {
        std::istringstream list(command({"pactl","list","short",kind}));std::string line;
        while(std::getline(list,line)) {
            std::istringstream fields(line);std::string index,found,driver;fields>>index>>found>>driver;
            if(found==name) {
                const auto expected=kind=="sinks"?"module-null-sink.c":"module-remap-source.c";
                if(driver!=expected && driver!="PipeWire") throw std::runtime_error("Reserved virtual audio name is already used: "+name);
                return; // Reuse devices left by a killed adapter; never unload another owner's modules.
            }
        }
        args.insert(args.begin(),{"pactl","load-module"});
        auto id=command(std::move(args));id.erase(id.find_last_not_of("\r\n ")+1);modules.push_back(id);
    }
    ~Devices(){for(auto i=modules.rbegin();i!=modules.rend();++i) try {command({"pactl","unload-module",*i});} catch(...) {}}
};
struct Pulse {
    pa_simple* stream=nullptr;
    Pulse(const std::string& device,bool capture) {
        pa_sample_spec spec{PA_SAMPLE_S16LE,12000,1};
        pa_buffer_attr attr{6000,960,0,480,480};int error=0;
        stream=pa_simple_new(nullptr,"QO100 Digital Audio",capture?PA_STREAM_RECORD:PA_STREAM_PLAYBACK,
            device.c_str(),capture?"TX2 external audio":"RX2 external audio",&spec,nullptr,&attr,&error);
        if(!stream) throw std::runtime_error(std::string("Cannot open virtual audio: ")+pa_strerror(error));
    }
    ~Pulse(){if(stream) pa_simple_free(stream);}
};
struct Identity {
    uint32_t session=0,epoch=0;uint64_t frequency=0;bool ready=false;
    bool operator==(const Identity& b) const {return session==b.session && epoch==b.epoch && frequency==b.frequency && ready==b.ready;}
};
unsigned port(const std::string& value) {
    if(value.empty() || value.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Expected a UDP port");
    const auto n=std::stoul(value);if(!n || n>65535) throw std::runtime_error("UDP port outside 1..65535");return n;
}
}
int main(int argc,char** argv) {
    try {
        bool always_active=false;unsigned audio_port=40914,bridge_port=40915;std::string prefix="qo100";
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") {std::cout<<"qo100_digital_audio [--port 40914] [--bridge-port 40915] [--device-prefix qo100] [--always-active]\n";return 0;}
            if(arg=="--always-active") {always_active=true;continue;}
            if(++i==argc) throw std::runtime_error("Missing argument for "+arg);
            if(arg=="--port") audio_port=port(argv[i]);
            else if(arg=="--bridge-port") bridge_port=port(argv[i]);
            else if(arg=="--device-prefix") prefix=argv[i];
            else throw std::runtime_error("Unknown argument: "+arg);
        }
        if(prefix.empty() || prefix.size()>48 || prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string::npos)
            throw std::runtime_error("Device prefix must contain 1..48 letters, digits or underscores");
        if(audio_port==bridge_port) throw std::runtime_error("Audio and bridge ports must differ");
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
        Fd udp(socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0));
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(audio_port);
        if(udp.fd<0 || bind(udp.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))) throw std::runtime_error("Cannot bind digital audio UDP port");
        auto remote=local;remote.sin_port=htons(bridge_port);
        if(connect(udp.fd,reinterpret_cast<sockaddr*>(&remote),sizeof(remote))) throw std::runtime_error("Cannot connect digital audio UDP socket");
        const auto lock_path="/tmp/qo100-digital-"+std::to_string(getuid())+"-"+prefix+".lock";
        Fd lock(open(lock_path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600));
        if(lock.fd<0 || flock(lock.fd,LOCK_EX|LOCK_NB)) throw std::runtime_error("Digital audio devices are already in use");
        Devices devices;
        const bool pipewire=command({"pactl","info"}).find("PipeWire")!=std::string::npos;
        const auto device_prefix=prefix=="qo100"?std::string{}:prefix+"_";
        const auto from=device_prefix+"fromQO100",to=device_prefix+"toQO100",internal=from+"_internal";
        for(const auto& name:{internal,to}) {
            std::vector<std::string> args{"module-null-sink","sink_name="+name,"rate=48000","channels=1","channel_map=mono",
                std::string("sink_properties=device.description=")+(name==internal?"fromQO100_internal":"toQO100")};
            if(!pipewire) args.push_back("norewinds=1"); // PulseAudio's default null-sink block is two seconds.
            devices.ensure("sinks",name,std::move(args));
        }
        devices.ensure("sources",from,{"module-remap-source","master="+internal+".monitor",
            "source_name="+from,"source_properties=device.description=fromQO100","channels=1","channel_map=mono","master_channel_map=mono"});
        std::condition_variable wake;
        PulseUsage usage(from,to,internal,[&]{wake.notify_all();});
        const auto used=[&]{return always_active?PulseUsage::State{true,true,true,true}:usage.state();};
        std::mutex mutex;Identity identity;
        qo100::TxChannel rx_clock(0);uint32_t rx_session=0,rx_epoch=0;
        auto last_rx=Clock::now()-std::chrono::seconds(10),last_state=last_rx;
        uint64_t nonce=(uint64_t(std::random_device{}())<<32)|std::random_device{}()|1;
        std::thread output([&] {
            try {
                std::unique_ptr<Pulse> playback;
                while(running) {
                    if(!used().rx) {
                        const bool reset=bool(playback);playback.reset();
                        std::unique_lock<std::mutex> guard(mutex);
                        if(reset) {rx_clock.reset(0);rx_session=rx_epoch=0;}
                        wake.wait_for(guard,std::chrono::milliseconds(100),[&]{return !running || used().rx;});
                        continue;
                    }
                    if(!playback) playback=std::make_unique<Pulse>(internal,false);
                    std::vector<float> samples;
                    {std::lock_guard<std::mutex> guard(mutex);samples=rx_clock.pull_audio(Clock::now()-last_rx<std::chrono::milliseconds(200));}
                    std::array<int16_t,240> pcm{};
                    for(size_t i=0;i<samples.size();++i) pcm[i]=int16_t(std::lround(std::clamp(samples[i],-1.f,1.f)*32767));
                    int error=0;if(pa_simple_write(playback->stream,pcm.data(),sizeof(pcm),&error)) throw std::runtime_error(pa_strerror(error));
                }
            } catch(const std::exception& e) {std::cerr<<"RX2 audio: "<<e.what()<<'\n';running=0;}
        });
        std::thread input([&] {
            try {
                Identity previous;uint64_t index=0;std::unique_ptr<Pulse> capture;
                while(running) {
                    Identity current;{std::lock_guard<std::mutex> guard(mutex);current=identity;
                        if(Clock::now()-last_state>std::chrono::seconds(1)) current.ready=false;}
                    if(!used().tx || !current.ready) {
                        capture.reset();
                        std::unique_lock<std::mutex> guard(mutex);
                        wake.wait_for(guard,std::chrono::milliseconds(50),[&]{return !running || (used().tx && identity.ready && Clock::now()-last_state<std::chrono::seconds(1));});
                        continue;
                    }
                    if(!capture) capture=std::make_unique<Pulse>(to+".monitor",true);
                    int error=0;
                    if(!(current==previous)) {
                        if(pa_simple_flush(capture->stream,&error)) throw std::runtime_error(pa_strerror(error));
                        // Readiness may briefly disappear without a new backend epoch.
                        // Only a new wire identity permits sample index zero again.
                        if(current.session!=previous.session || current.epoch!=previous.epoch || current.frequency!=previous.frequency) index=0;
                        previous=current;
                    }
                    std::array<int16_t,240> pcm{};
                    if(pa_simple_read(capture->stream,pcm.data(),sizeof(pcm),&error)) throw std::runtime_error(pa_strerror(error));
                    {std::lock_guard<std::mutex> guard(mutex);if(!(identity==current) || !current.ready || !used().tx) continue;}
                    auto packet=bridge::header(8,qo100::tx2_stream,current.epoch,current.session);packet.resize(544);
                    bridge::put(packet,24,index,8);bridge::put(packet,32,current.frequency,8);bridge::put(packet,40,12000,4);
                    bridge::put(packet,52,240,2);bridge::put(packet,54,2,2);bridge::put(packet,56,240,4);
                    for(size_t i=0;i<pcm.size();++i) bridge::put(packet,64+2*i,uint16_t(pcm[i]),2);
                    (void)send(udp.fd,packet.data(),packet.size(),0);index+=240;
                }
            } catch(const std::exception& e) {std::cerr<<"TX2 audio: "<<e.what()<<'\n';running=0;}
        });
        std::cout<<"Digital audio: Input fromQO100="<<from<<"; Output toQO100="<<to<<"; UDP "<<audio_port<<'/'<<bridge_port<<std::endl;
        try {
            auto next_hello=Clock::now(),next_usage=next_hello;
            unsigned previous_usage=UINT32_MAX;bool previous_tx=false;
            std::array<uint8_t,1201> buffer{};
            while(running) {
                const auto users=used();
                const unsigned mask=(users.rx?1u:0u)|(users.tx?2u:0u);
                if(users.available && mask!=previous_usage) {
                    std::cout<<"Digital audio users: RX2="<<users.rx<<" TX2="<<users.tx
                        <<(users.known?"":" (usage detection unavailable; keeping audio active)")<<std::endl;
                    if(users.tx && !previous_tx) {
                        // Also supports older bridges: every new TX user gets a
                        // fresh epoch before sample index zero, never stale audio.
                        if(++nonce==0)++nonce;
                        std::lock_guard<std::mutex> guard(mutex);identity.ready=false;
                        last_state=Clock::now()-std::chrono::seconds(10);next_hello={};
                    }
                    previous_tx=users.tx;previous_usage=mask;next_usage={};wake.notify_all();
                }
                Identity current;{std::lock_guard<std::mutex> guard(mutex);current=identity;}
                if(users.available && current.session && Clock::now()>=next_usage) {
                    auto activity=bridge::header(20,qo100::tx2_stream,0,current.session);
                    bridge::put(activity,16,nonce,8);bridge::put(activity,48,mask,4);
                    (void)send(udp.fd,activity.data(),activity.size(),0);next_usage=Clock::now()+std::chrono::milliseconds(500);
                }
                bool stale;{std::lock_guard<std::mutex> guard(mutex);stale=Clock::now()-last_state>std::chrono::seconds(1);}
                if(stale && Clock::now()>=next_hello) {
                    auto hello=bridge::header(19,qo100::tx2_stream);bridge::put(hello,16,nonce,8);
                    (void)send(udp.fd,hello.data(),hello.size(),0);next_hello=Clock::now()+std::chrono::milliseconds(250);
                }
                pollfd pfd{udp.fd,POLLIN,0};if(poll(&pfd,1,50)<0 && errno!=EINTR) throw std::runtime_error("Digital UDP poll failed");
                for(int budget=0;budget<64;++budget) {
                    const auto n=recv(udp.fd,buffer.data(),buffer.size(),0);if(n<0) break;
                    bridge::Bytes p(buffer.begin(),buffer.begin()+n);
                    if(!bridge::valid(p) || !bridge::get(p,60,4)) continue;
                    if(p[5]==7 && n==64 && bridge::get(p,8,4)==qo100::tx2_stream && bridge::get(p,16,8)==nonce) {
                        Identity next{uint32_t(bridge::get(p,60,4)),uint32_t(bridge::get(p,24,8)),bridge::get(p,32,8),
                            bridge::get(p,48,4)!=0}; // Command errors do not disable an unchanged valid frequency.
                        std::lock_guard<std::mutex> guard(mutex);
                        if(next.session==identity.session && int32_t(next.epoch-identity.epoch)<0) continue;
                        if(!(next==identity))next_usage={};
                        identity=next;last_state=Clock::now();wake.notify_all();
                    } else if(p[5]==5 && n==544 && bridge::get(p,8,4)==2 && bridge::get(p,40,4)==12000 &&
                        !bridge::get(p,44,8) && bridge::get(p,52,2)==240 && bridge::get(p,54,2)==2 && bridge::get(p,56,4)==240) {
                        if(!used().rx) continue;
                        std::vector<int16_t> pcm(240);for(size_t i=0;i<pcm.size();++i) pcm[i]=int16_t(bridge::get(p,64+2*i,2));
                        std::lock_guard<std::mutex> guard(mutex);
                        const auto session=bridge::get(p,60,4),epoch=bridge::get(p,12,4);
                        if(session!=rx_session || epoch!=rx_epoch) {rx_clock.reset(0);rx_session=session;rx_epoch=epoch;}
                        if(rx_clock.add(bridge::get(p,24,8),pcm)) last_rx=Clock::now();
                    }
                }
            }
        } catch(...) {running=0;wake.notify_all();input.join();output.join();throw;}
        wake.notify_all();input.join();output.join();return 0;
    } catch(const std::exception& e) {std::cerr<<"Digital audio: "<<e.what()<<'\n';return 1;}
}
