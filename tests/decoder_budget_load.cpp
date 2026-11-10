// Deliberately expensive test-only workload: threads plus a separate child PGID.
#include "cpu_budget.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
int main(int argc,char** argv) {
    if(argc!=4)return 2;
    const unsigned percent=unsigned(std::atoi(argv[1])),threads=unsigned(std::atoi(argv[2]));
    const double seconds=std::atof(argv[3]);
    if(auto exit=qo100::decoder_cpu_budget(percent))return *exit;
    if(getpriority(PRIO_PROCESS,0)!=19)return 3;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::duration<double>(seconds);
    if(threads==0) {std::this_thread::sleep_until(deadline);return 0;}
    const auto busy=[&] {
        volatile uint64_t value=1;
        while(std::chrono::steady_clock::now()<deadline)for(unsigned i=0;i<1000;++i)value=value*1664525+1013904223;
    };
    const auto parent=getpid();pid_t child=-1;int status=0;
    std::thread spawning_worker([&] {
        child=fork();
        if(child==0) {
            setpgid(0,0);prctl(PR_SET_PDEATHSIG,SIGKILL);if(getppid()!=parent)_exit(125);
            busy();_exit(0);
        }
        busy();
        // PR_SET_PDEATHSIG watches the creating thread. Reap its solver before
        // this thread exits, just as the FT8 decode worker does in production.
        if(child>0)while(waitpid(child,&status,0)<0 && errno==EINTR){}
    });
    std::vector<std::thread> workers;
    for(unsigned i=1;i<threads;++i)workers.emplace_back(busy);
    busy();for(auto& worker:workers)worker.join();spawning_worker.join();
    if(child<0)return 4;
    std::cout<<"budget test finished\n";
    return WIFEXITED(status)?WEXITSTATUS(status):5;
}
