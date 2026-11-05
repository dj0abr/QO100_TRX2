#include "qo100/rx_channel.hpp"
#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <time.h>
#include <vector>
using C=std::complex<float>;constexpr double pi=3.14159265358979323846;
volatile float sink;
double cpu(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(){
 std::vector<C> iq(600000);for(size_t i=0;i<iq.size();++i)iq[i]=std::polar(.25f,float(2*pi*125000*i/600000));
 std::vector<int16_t> pcm(240);for(size_t i=0;i<pcm.size();++i)pcm[i]=int16_t(16000*std::cos(2*pi*1000*i/12000));
 std::vector<double> rxtimes,txtimes;
 for(int repetition=0;repetition<7;++repetition){
  qo100::RxChannel rx1(150000,[](C x){sink=x.real();},[](const auto& p,uint64_t){sink=p[0];});
  qo100::RxChannel rx2(-150000,[](C){},[](const auto& p,uint64_t){sink=p[0];});
  double start=cpu();for(C x:iq){rx1.push(x);rx2.push(x);}rxtimes.push_back((cpu()-start)*1000);
  qo100::TxChannel tx(150000);uint64_t index=0;for(int i=0;i<6;++i){tx.add(index,pcm);index+=240;}
  start=cpu();for(int i=0;i<100;++i){if(i){tx.add(index,pcm);index+=240;}auto y=tx.render(true);sink=y[12000].real();}txtimes.push_back((cpu()-start)*500);
 }
 std::sort(rxtimes.begin(),rxtimes.end());std::sort(txtimes.begin(),txtimes.end());
 std::cout<<"Two RX channels: "<<rxtimes[3]<<" CPU ms/s; TX: "<<txtimes[3]<<" CPU ms/s\n";
}
