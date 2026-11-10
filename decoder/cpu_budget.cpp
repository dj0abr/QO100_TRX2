// SPDX-License-Identifier: GPL-3.0-only
#include "cpu_budget.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <map>
#include <poll.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace qo100 {
namespace {
using Clock=std::chrono::steady_clock;
volatile sig_atomic_t stopping=0;
void stop(int) { stopping=1; }
double cpu(clockid_t id) {
    timespec t{};
    if(clock_gettime(id,&t)) return -1;
    return t.tv_sec+t.tv_nsec*1e-9;
}
std::string identity(pid_t pid) {
    std::ifstream in("/proc/"+std::to_string(pid)+"/stat");std::string line;
    if(!std::getline(in,line))return {};
    const auto end=line.rfind(')');if(end==std::string::npos)return {};
    std::istringstream fields(line.substr(end+2));std::string value;
    for(unsigned i=0;i<=19;++i)if(!(fields>>value))return {};
    return value; // starttime, after the parenthesized comm field
}
struct Process {clockid_t clock{};double last=0;int fd=-1;std::string identity;};
class Tree {
public:
    explicit Tree(pid_t root):root_(root) {add(root);}
    ~Tree() {for(const auto& item:processes_)if(item.second.fd>=0)close(item.second.fd);}
    void discover() {
        // Children can be spawned from any thread (FT8 spawns on its worker).
        std::set<pid_t> visited;
        std::vector<pid_t> scan{getpid()}; // also find adopted orphan solvers
        for(const auto& item:processes_)scan.push_back(item.first);
        for(size_t i=0;i<scan.size();++i) {
            const auto pid=scan[i];
            if(!visited.insert(pid).second) continue;
            const auto path="/proc/"+std::to_string(pid)+"/task";
            DIR* tasks=opendir(path.c_str());
            if(!tasks) continue;
            while(const auto* task=readdir(tasks)) {
                if(task->d_name[0]=='.') continue;
                std::ifstream children(path+'/'+task->d_name+"/children");
                pid_t child;
                while(children>>child) {if(add(child))scan.push_back(child);}
            }
            closedir(tasks);
        }
    }
    double account() {
        double used=0;
        for(auto it=processes_.begin();it!=processes_.end();) {
            const double now=cpu(it->second.clock);
            if(now<0 || !alive(it->first,it->second)) {
                if(it->second.fd>=0)close(it->second.fd);
                it=processes_.erase(it);continue;
            }
            used+=std::max(0.,now-it->second.last);
            it->second.last=now;++it;
        }
        return used;
    }
    void signal(int number) {
        // All these PIDs were discovered below our own unreaped worker.
        // Group signal also catches children created between discovery passes.
        const auto root=processes_.find(root_);
        if(root!=processes_.end() && alive(root_,root->second))kill(-root_,number);
        for(const auto& item:processes_) {
            if(!alive(item.first,item.second))continue;
#ifdef SYS_pidfd_send_signal
            if(item.second.fd>=0) {syscall(SYS_pidfd_send_signal,item.second.fd,number,nullptr,0);continue;}
#endif
            kill(item.first,number);
        }
    }
private:
    bool alive(pid_t pid,const Process& p) const {
        if(p.fd>=0) {pollfd fd{p.fd,POLLIN,0};return poll(&fd,1,0)==0;}
        return !p.identity.empty() && identity(pid)==p.identity;
    }
    bool add(pid_t pid) {
        if(pid==getpid() || processes_.count(pid)) return false;
        clockid_t id;
        if(clock_getcpuclockid(pid,&id))return false;
        int fd=-1;
#ifdef SYS_pidfd_open
        fd=int(syscall(SYS_pidfd_open,pid,0));
#endif
        processes_.emplace(pid,Process{id,0,fd,identity(pid)});return true;
    }
    pid_t root_;
    std::map<pid_t,Process> processes_;
};
int result(int status) {
    return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
}
std::optional<int> decoder_cpu_budget(unsigned percent) {
    if(!percent || percent>100) throw std::runtime_error("--cpu-percent must be 1..100");
    const auto parent=getpid();
    const auto worker=fork();
    if(worker<0) throw std::runtime_error("Cannot start decoder CPU guard");
    if(worker==0) {
        setpgid(0,0);
        prctl(PR_SET_PDEATHSIG,SIGKILL);
        if(getppid()!=parent) _exit(125);
        if(setpriority(PRIO_PROCESS,0,19)) {
            std::cerr<<"Cannot lower decoder priority: "<<std::strerror(errno)<<'\n';_exit(126);
        }
        sched_param priority{};
        // Linux permits lowering our own scheduling class without root.
        if(sched_setscheduler(0,SCHED_IDLE,&priority))
            std::cerr<<"Decoder uses nice 19; SCHED_IDLE unavailable: "<<std::strerror(errno)<<'\n';
        setenv("QO100_DECODER_GUARDED","1",1);
        setenv("OMP_NUM_THREADS","1",1);setenv("OPENBLAS_NUM_THREADS","1",1);
        return std::nullopt;
    }
    setpgid(worker,worker);
    // Reap/stop orphaned solver children even if a decoder exits abnormally.
    prctl(PR_SET_CHILD_SUBREAPER,1);
    stopping=0;
    struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,nullptr);sigaction(SIGTERM,&action,nullptr);sigaction(SIGHUP,&action,nullptr);
    Tree tree(worker);
    // Small reserve covers final accounting and process exit overhead.
    const double share=percent/100.*.97;
    // 20-ms period, at most 2 ms CPU credit at the default 10%. Idle time never
    // buys a full-core multi-second decode later. Debt is carried across periods.
    const double capacity=.020*share;
    double credit=capacity,self=cpu(CLOCK_PROCESS_CPUTIME_ID);
    auto last=Clock::now(),discovery=last;
    auto last_discovery=last-std::chrono::milliseconds(20);
    bool paused=false;unsigned idle=0;
    int status=0;
    auto reap_descendants=[&] {
        tree.discover();tree.signal(SIGCONT);tree.signal(SIGKILL);
        const auto end=Clock::now()+std::chrono::milliseconds(500);
        while(Clock::now()<end) {
            int ignored=0;
            const auto pid=waitpid(-1,&ignored,WNOHANG);
            if(pid>0)continue;
            if(pid<0 && errno==ECHILD)return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    std::cerr<<"Decoder CPU budget: "<<percent<<"% of one core (whole process tree), nice 19/SCHED_IDLE\n";
    for(;;) {
        double work=tree.account();
        // No 2-kHz /proc scanning while the decoder is waiting for audio. Scan
        // on CPU activity (new FT8 solver), periodically, and at stop boundaries.
        const auto now=Clock::now();
        if(now>=discovery || (work>.000025 && now-last_discovery>=std::chrono::milliseconds(2))) {
            tree.discover();last_discovery=Clock::now();discovery=last_discovery+std::chrono::milliseconds(20);
            work+=tree.account();
            if(paused) tree.signal(SIGSTOP);
        }
        const double own=cpu(CLOCK_PROCESS_CPUTIME_ID);
        credit=std::min(capacity,credit+std::chrono::duration<double>(now-last).count()*share)
            -work-std::max(0.,own-self);
        last=now;self=own;
        const auto waited=waitpid(worker,&status,WNOHANG);
        if(waited==worker) {reap_descendants();return result(status);}
        if(waited<0 && errno!=EINTR) {tree.signal(SIGKILL);throw std::runtime_error("Cannot wait for decoder worker");}
        if(stopping) break;
        if(credit<=0 && !paused) {tree.signal(SIGSTOP);tree.discover();tree.signal(SIGSTOP);paused=true;}
        if(paused && credit>=capacity*.8) {tree.signal(SIGCONT);paused=false;}
        // Accounting clocks have nanosecond resolution, unlike /proc tick counts.
        // Active bursts are sampled at 0.5 ms; while paused the guard sleeps.
        idle=work<.000025?std::min(10u,idle+1):0;
        const double sleep=paused?std::clamp((capacity*.8-credit)/share,.0005,.100):(idle>=3?.005:.0005);
        std::this_thread::sleep_for(std::chrono::duration<double>(sleep));
    }
    // A stopped process cannot handle SIGTERM: always continue before shutdown.
    tree.discover();tree.signal(SIGCONT);tree.signal(SIGTERM);
    const auto deadline=Clock::now()+std::chrono::seconds(2);
    while(Clock::now()<deadline) {
        if(waitpid(worker,&status,WNOHANG)==worker) {reap_descendants();return result(status);}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    tree.discover();tree.signal(SIGKILL);
    while(waitpid(worker,&status,0)<0 && errno==EINTR) {}
    reap_descendants();
    return 0;
}
}
