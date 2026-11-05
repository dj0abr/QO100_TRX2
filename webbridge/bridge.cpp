#include "bridge.hpp"
#include <charconv>
#include <cmath>
#include <iostream>
#include <sstream>

namespace bridge {
namespace {
unsigned group(uint8_t type,unsigned stream) {return unsigned(type)*8+stream;}
uint64_t integer(const Json& j,const char* key,uint64_t maximum=UINT32_MAX) {
    const auto& value=j.at(key);uint64_t result=0;
    if(value.is_string()) {
        const auto s=value.get<std::string>();const auto r=std::from_chars(s.data(),s.data()+s.size(),result);
        if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size()) throw std::runtime_error(std::string("Invalid ")+key);
    } else if(value.is_number_unsigned()) result=value.get<uint64_t>();
    else if(value.is_number_integer() && value.get<int64_t>()>=0) result=uint64_t(value.get<int64_t>());
    else throw std::runtime_error(std::string("Expected unsigned integer: ")+key);
    if(result>maximum) throw std::runtime_error(std::string("Out of range: ")+key);
    return result;
}
Json snapshot_values(const Bytes& p) {
    if(get(p,44,4)!=p.size()-64 || get(p,52,2)!=p.size()-64 || get(p,56,4)!=p.size()-64)
        throw std::runtime_error("Invalid settings payload length");
    Json values=Json::object();std::istringstream in(std::string(p.begin()+64,p.end()));std::string line;
    while(std::getline(in,line)) {
        const auto eq=line.find('=');if(eq==std::string::npos || eq==0) throw std::runtime_error("Invalid settings snapshot");
        values[line.substr(0,eq)]=line.substr(eq+1);
    }
    return values;
}
std::shared_ptr<const Bytes> encoded(const Json& j) {
    const auto s=j.dump();return std::make_shared<const Bytes>(s.begin(),s.end());
}
}
Bridge::Bridge(net::io_context& io,Config config):config_(std::move(config)),udp_(io),timer_(io) {
    udp_.open(net::ip::udp::v4());
    udp_.set_option(net::socket_base::receive_buffer_size(4*1024*1024));
    udp_.bind({net::ip::make_address_v4("127.0.0.1"),0});
    udp_.connect({net::ip::make_address_v4("127.0.0.1"),config_.backend_port});
    state_={{"type","state"},{"session",0},{"ready",false},{"settings",Json::object()},
        {"revision","0"},{"flags",0},{"rx",Json::object()},{"tx",Json::object()},
        {"filter",Json::object()},{"compressor",Json::object()},{"nr",Json::object()},{"loop",Json::object()}};
}
void Bridge::start() {receive();tick();}
void Bridge::send_udp(Bytes p) {
    // The socket is connected to localhost; each handler retains its own buffer.
    auto data=std::make_shared<Bytes>(std::move(p));
    udp_.async_send(net::buffer(*data),[data](auto,size_t){});
}
void Bridge::receive() {
    udp_.async_receive(net::buffer(receive_buffer_),[this](auto ec,size_t n) {
        if(!ec) {
            try {packet(Bytes(receive_buffer_.begin(),receive_buffer_.begin()+n));}
            catch(const std::exception& e) {std::cerr<<"Ignored UDP packet: "<<e.what()<<'\n';}
        }
        if(ec!=net::error::operation_aborted) receive();
    });
}
bool Bridge::ready() const {return initialized_ && !barrier_ && !hardware_change_ && !stopping_;}
void Bridge::text(uint64_t client,const Json& j) {
    const auto it=clients_.find(client);if(it!=clients_.end()) it->second.peer->send(encoded(j),false);
}
void Bridge::broadcast(const Json& j) {
    const auto bytes=encoded(j);
    // send() may synchronously disconnect a slow peer; traverse a copy.
    const auto copy=clients_;for(const auto& c:copy) c.second.peer->send(bytes,false);
}
void Bridge::error(uint64_t client,const Json& id,const std::string& code,const std::string& message) {
    text(client,{{"type","error"},{"id",id},{"code",code},{"message",message}});
}
void Bridge::roles() {
    const auto copy=clients_;
    for(const auto& c:copy) text(c.first,{{"type","role"},{"role",c.first==owner_?"operator":"viewer"},
        {"client_id",c.first},{"owner_present",owner_!=0},{"clients",clients_.size()},{"idle_seconds",config_.idle_seconds}});
}
uint64_t Bridge::join(std::shared_ptr<Peer> peer) {
    if(stopping_ || clients_.size()>=config_.max_clients) {peer->close("Server full");return 0;}
    const auto id=++next_client_;const auto now=Clock::now();clients_[id]={std::move(peer),now,now};
    if(!owner_) owner_=id; // Only a NEW connection can acquire a vacant seat.
    roles();state_["ready"]=ready();text(id,state_);
    std::cout<<"Browser "<<id<<" joined as "<<(owner_==id?"operator":"viewer")<<'\n'<<std::flush;
    return id;
}
void Bridge::leave(uint64_t id) {
    const bool owned=id && id==owner_;clients_.erase(id);
    if(owned) revoke("disconnected");else roles();
}
void Bridge::revoke(const std::string& reason) {
    const auto old=owner_;owner_=0;
    for(auto it=pending_.begin();it!=pending_.end();) {
        if(it->second.client==old && old) it=pending_.erase(it);else ++it;
    }
    safety();roles();
    if(old) std::cout<<"Operator "<<old<<" revoked: "<<reason<<'\n'<<std::flush;
}
void Bridge::control(Bytes p,uint8_t reply,uint64_t client,Json id,std::string command,bool safety_command) {
    const auto key=group(p[5],unsigned(get(p,8,4)));
    auto old=pending_.find(key);
    if(old!=pending_.end() && old->second.client)
        error(old->second.client,old->second.id,"superseded","Replaced by a newer command");
    put(p,12,++counters_[key],4);
    put(p,60,p[5]==17 && !get(p,54,2)?0:session_,4);
    last_controls_[key]=p;
    Pending q{std::move(p),reply,client,std::move(id),std::move(command),{},Clock::now(),safety_command};
    q.sent=Clock::now();send_udp(q.packet);pending_[key]=std::move(q);
}
void Bridge::discover() {
    if(pending_.count(group(17,0))) return;
    control(header(17),18); // Discovery queries always use session=0.
}
void Bridge::tx_off() {
    ptt_requested_=false;ptt_until_={};
    if(!session_) return;
    auto p=header(6,3);uint64_t hz=2400250000ULL;
    if(state_["tx"].contains("frequency_hz")) hz=state_["tx"]["frequency_hz"].get<uint64_t>();
    else if(state_["settings"].contains("initial_tx_hz")) hz=std::stoull(state_["settings"]["initial_tx_hz"].get<std::string>());
    put(p,32,hz,8);control(std::move(p),7,0,nullptr,"",true);
}
void Bridge::safety() {
    barrier_=true;off_ack_=normal_ack_=false;dirty_=true;tx_off();
    if(session_) {auto p=header(13);put(p,54,1,2);control(std::move(p),14,0,nullptr,"",true);}
}
void Bridge::reset_backend() {
    for(const auto& q:pending_) if(q.second.client) error(q.second.client,q.second.id,"backend_reset","Backend session lost");
    pending_.clear();last_controls_.clear();session_=0;initialized_=false;
    ptt_requested_=false;hardware_change_=false;barrier_=true;off_ack_=normal_ack_=false;
    state_["session"]=0;state_["ready"]=false;
    for(const auto& key:{"rx","tx","filter","compressor","nr","loop"}) state_[key]=Json::object();
    next_settings_={};dirty_=true;
}
void Bridge::bootstrap() {
    initialized_=true;
    for(unsigned stream:{1u,2u}) {
        const auto key=stream==1?"initial_rx_hz":"initial_rx2_hz";
        auto p=header(3,stream);put(p,32,std::stoull(state_["settings"].at(key).get<std::string>()),8);
        control(std::move(p),4);
    }
    for(const auto& pair:{std::pair<uint8_t,uint8_t>{9,10},{11,12},{15,16}})
        control(header(pair.first,pair.first==11?3:1),pair.second);
    safety();
}
void Bridge::update_state(const Bytes& p) {
    const auto type=p[5];const auto stream=unsigned(get(p,8,4));
    Json v={{"status",get(p,54,2)}};
    if(type==18) {
        state_["settings"]=snapshot_values(p);state_["revision"]=std::to_string(get(p,24,8));state_["flags"]=get(p,48,4);
        state_["settings_status"]=get(p,54,2);return;
    }
    if(type==4 || type==7) {
        v["frequency_hz"]=get(p,32,8);v["epoch"]=get(p,24,8);
        if(type==4) state_["rx"][std::to_string(stream)]=v;
        else {
            v["ptt"]=get(p,48,4)!=0;v["audio_fifo"]=get(p,44,4);v["pluto_fifo"]=get(p,52,2);
            const auto raw=get(p,56,4);v["ppm"]=raw>=0x80000000ULL?int64_t(raw)-0x100000000LL:int64_t(raw);
            state_["tx"]=v;
        }
    } else if(type==10) {
        v["bandwidth_hz"]=get(p,44,4);v["center_hz"]=get(p,32,8);state_["filter"]=v;
    } else if(type==12) {
        v["percent"]=get(p,44,4)/10.;v["compression_db"]=get(p,48,4)/1000.;v["limiting_db"]=get(p,56,4)/1000.;state_["compressor"]=v;
    } else if(type==16) {v["percent"]=get(p,44,4);state_["nr"]=v;}
    else if(type==14) {
        v["mode"]=get(p,44,4);v["target"]=get(p,48,4);v["generation"]=get(p,24,8);
        v["timestamp_ns"]=std::to_string(get(p,16,8));v["rx_lo_hz"]=get(p,32,8);state_["loop"]=v;
    }
}
void Bridge::packet(Bytes p) {
    if(!valid(p)) return;
    const auto sid=uint32_t(get(p,60,4));if(!sid) return;
    const auto type=p[5];const auto stream=unsigned(get(p,8,4));
    if(sid!=session_) {
        if(type!=18 || !pending_.count(group(17,0)) ||
            get(p,12,4)!=get(pending_.at(group(17,0)).packet,12,4)) {
            next_settings_={};
            return;
        }
        if(session_) reset_backend();
        session_=sid;state_["session"]=sid;dirty_=true;
    }
    last_rx_=Clock::now();
    if(type==2 || type==5) {
        if(!initialized_) return;
        if(type==2 && (stream>1 || get(p,54,2)!=1 || !get(p,52,2) || p.size()!=64+2*get(p,52,2))) return;
        if(type==5 && ((stream!=1 && stream!=2 && stream!=4) || p.size()!=544 ||
            get(p,40,4)!=12000 || get(p,52,2)!=240 || get(p,54,2)!=2 || get(p,56,4)!=240)) return;
        auto bytes=std::make_shared<const Bytes>(std::move(p));const auto copy=clients_;
        for(const auto& c:copy) c.second.peer->send(bytes,true);
        return;
    }
    uint8_t command=0;
    switch(type) {case 4:command=3;break;case 7:command=6;break;case 10:command=9;break;
        case 12:command=11;break;case 14:command=13;break;case 16:command=15;break;case 18:command=17;break;default:return;}
    const auto key=group(command,stream);
    if(get(p,12,4)!=counters_[key]) return; // Reordered replies cannot roll state back.
    update_state(p);dirty_=true;
    auto found=pending_.find(key);
    const bool final=!(type==18 && (get(p,48,4)&2)) && !(type==14 && get(p,54,2)==3);
    if(type==18 && final) hardware_change_=false;
    if(found!=pending_.end() && found->second.reply==type && final) {
        const auto q=found->second;pending_.erase(found);
        if(q.client && q.client==owner_) text(q.client,{{"type","ack"},{"id",q.id},{"command",q.command},
            {"status",get(p,54,2)},{"revision",state_["revision"]},{"flags",state_["flags"]}});
        if(q.safety && type==7 && !get(p,54,2) && !get(p,48,4)) off_ack_=true;
        if(q.safety && type==14 && !get(p,54,2) && !get(p,44,4)) normal_ack_=true;
    }
    if(type==18 && !initialized_ && !(get(p,48,4)&2) && state_["settings"].contains("initial_rx_hz")) bootstrap();
    else if(type==18 && initialized_ && !(get(p,48,4)&2)) {
        // A settings patch can change DSP values without emitting their dedicated ACKs.
        for(const auto& pair:{std::pair<uint8_t,uint8_t>{9,10},{11,12},{15,16}}) {
            const auto stream_id=pair.first==11?3:1;
            if(!pending_.count(group(pair.first,stream_id))) control(header(pair.first,stream_id),pair.second);
        }
    }
    if(type==7 && get(p,54,2) && ptt_requested_) tx_off();
    if(type==7 && !pending_.count(group(6,3)) && !get(p,48,4)) ptt_requested_=false;
    if(barrier_ && off_ack_ && normal_ack_ && initialized_) {barrier_=false;dirty_=true;}
}
void Bridge::message(uint64_t client,const Bytes& bytes,bool binary) {
    if(!clients_.count(client)) return;
    Json id=nullptr;
    try {
        if(binary) {
            if(client!=owner_) {error(client,id,"read_only","Viewer cannot transmit");return;}
            if(!ready() || !ptt_requested_ || Clock::now()>ptt_until_ || !state_["tx"].value("ptt",false)) return;
            if(!valid(bytes) || bytes.size()!=544 || bytes[5]!=8 || get(bytes,8,4)!=3 || get(bytes,16,8) ||
                get(bytes,40,4)!=12000 || get(bytes,44,8) || get(bytes,52,2)!=240 || get(bytes,54,2)!=2 || get(bytes,56,4)!=240 ||
                get(bytes,24,8)%240 || get(bytes,60,4)!=session_ || get(bytes,12,4)!=state_["tx"].at("epoch").get<uint32_t>() ||
                get(bytes,32,8)!=state_["tx"].at("frequency_hz").get<uint64_t>())
                throw std::runtime_error("Invalid TX_AUDIO identity, index, or PCM format");
            send_udp(bytes);return;
        }
        const auto j=Json::parse(bytes);if(!j.is_object()) throw std::runtime_error("Expected JSON object");
        id=j.value("id",Json(nullptr));if(!id.is_null()) integer(j,"id");
        const auto type=j.at("type").get<std::string>();
        if(type=="state") {state_["ready"]=ready();text(client,state_);return;}
        if(type=="heartbeat") {clients_.at(client).heartbeat=Clock::now();return;}
        if(client!=owner_) {error(client,id,"read_only","Only the operator can send commands");return;}
        if(type=="activity") {clients_.at(client).activity=Clock::now();return;}
        if(type=="ptt_keepalive") {if(ptt_requested_) ptt_until_=Clock::now()+std::chrono::milliseconds(750);return;}
        if(!ready()) {error(client,id,"not_ready","Waiting for backend and confirmed safe state");return;}
        const bool query=j.value("query",false);Bytes p;uint8_t reply=0;
        if(type=="tune") {
            const auto stream=integer(j,"stream",2);if(!stream) throw std::runtime_error("RX stream must be 1 or 2");
            p=header(3,stream);put(p,32,integer(j,"frequency_hz",UINT64_MAX),8);reply=4;
        } else if(type=="tx") {
            p=header(6,3);put(p,32,integer(j,"frequency_hz",6000000000ULL),8);
            const auto enabled=j.at("ptt").get<bool>();
            if(enabled && (state_["flags"].get<unsigned>()&6)) throw std::runtime_error("Hardware offline or changing");
            put(p,54,enabled?1:0,2);reply=7;ptt_requested_=enabled;
            ptt_until_=enabled?Clock::now()+std::chrono::milliseconds(750):Clock::time_point{};
            next_tx_=Clock::now()+std::chrono::milliseconds(400);
        } else if(type=="filter") {
            p=header(9,1);reply=10;
            if(!query) {put(p,32,integer(j,"center_hz"),8);put(p,44,integer(j,"bandwidth_hz"),4);}
        } else if(type=="compressor") {
            p=header(11,3);reply=12;
            if(!query) {
                const auto percent=j.at("percent").get<double>();
                if(!std::isfinite(percent) || percent<0 || percent>140) throw std::runtime_error("Compressor range: 0..140 percent");
                put(p,44,uint32_t(std::lround(percent*10)),4);
            }
        } else if(type=="nr") {p=header(15,1);reply=16;if(!query) put(p,44,integer(j,"percent",100),4);}
        else if(type=="loop") {p=header(13);reply=14;if(!query) put(p,44,integer(j,"mode",2),4);}
        else if(type=="settings_get" || type=="settings_set") {
            const auto busy=pending_.find(group(17,0));
            if(busy!=pending_.end()) {error(client,id,"busy","Settings request pending; retry after its reply");return;}
            p=header(17);reply=18;
            if(type=="settings_set") {
                put(p,24,integer(j,"revision",UINT64_MAX),8);put(p,54,1,2);
                const auto& values=j.at("values");if(!values.is_object() || values.empty()) throw std::runtime_error("Expected nonempty settings object");
                std::string payload;
                for(auto i=values.begin();i!=values.end();++i) {
                    if(i.key().empty() || i.key().find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)
                        throw std::runtime_error("Invalid settings key");
                    if(!i->is_string() && !i->is_number()) throw std::runtime_error("Settings values must be strings or numbers");
                    const auto value=i->is_string()?i->get<std::string>():i->dump();
                    if(value.find_first_of("\r\n\0",0,3)!=std::string::npos) throw std::runtime_error("Invalid settings value");
                    payload+=i.key()+"="+value+"\n";
                }
                if(payload.size()>1136) throw std::runtime_error("Settings payload exceeds 1136 bytes");
                put(p,44,payload.size(),4);put(p,52,payload.size(),2);put(p,56,payload.size(),4);
                p.insert(p.end(),payload.begin(),payload.end());
                for(const auto* key:{"pluto_connection","pluto_ip","lnb_reference_hz","rx_correction_hz",
                    "tx_correction_hz","pluto_tx_base_hz","tx_attenuation_db","initial_tx_hz"}) {
                    if(values.contains(key)) {hardware_change_=true;safety();break;}
                }
            }
        } else throw std::runtime_error("Unknown command: "+type);
        if(reply==10 || reply==12 || reply==16 || reply==14) {
            put(p,54,query?0:1,2);if(!query && reply!=14) put(p,40,12000,4);
        }
        // Changing a test loop invalidates any outstanding transmit permission.
        if(type=="loop" && !query) {tx_off();}
        control(std::move(p),reply,client,id,type);
    } catch(const std::exception& e) {error(client,id,"invalid",e.what());}
}
void Bridge::tick() {
    const auto now=Clock::now();
    if(now>=next_sub_) {send_udp(header(1));next_sub_=now+std::chrono::seconds(2);}
    if(session_ && now-last_rx_>std::chrono::seconds(4)) reset_backend();
    if(now>=next_settings_) {discover();next_settings_=now+std::chrono::seconds(2);}
    if(owner_ && clients_.count(owner_)) {
        const auto c=clients_.at(owner_);
        const bool idle=now-c.activity>=std::chrono::seconds(config_.idle_seconds);
        if(idle || now-c.heartbeat>std::chrono::seconds(6)) {
            const auto id=owner_;revoke(idle?"idle":"heartbeat_timeout");
            text(id,{{"type","closed"},{"reason",idle?"idle":"heartbeat_timeout"}});
            c.peer->close(idle?"idle":"heartbeat_timeout");
        }
    }
    if(ptt_requested_ && now>ptt_until_) tx_off();
    if(initialized_ && now>=next_rx_) {
        for(unsigned stream:{1u,2u}) {
            const auto key=group(3,stream);
            if(last_controls_.count(key) && !pending_.count(key)) send_udp(last_controls_.at(key));
        }
        next_rx_=now+std::chrono::seconds(1);
    }
    if(ptt_requested_ && now>=next_tx_ && !pending_.count(group(6,3))) {
        const auto it=last_controls_.find(group(6,3));if(it!=last_controls_.end()) send_udp(it->second);
        next_tx_=now+std::chrono::milliseconds(400);
    }
    for(auto it=pending_.begin();it!=pending_.end();) {
        auto& q=it->second;
        if(!q.safety && now-q.started>std::chrono::seconds(12)) {
            if(q.client) error(q.client,q.id,"timeout","No final backend reply within 12 seconds");
            it=pending_.erase(it);continue;
        }
        if(now-q.sent>=std::chrono::milliseconds(250)) {send_udp(q.packet);q.sent=now;}
        ++it;
    }
    if(dirty_) {state_["ready"]=ready();broadcast(state_);dirty_=false;}
    timer_.expires_after(std::chrono::milliseconds(100));timer_.async_wait([this](auto ec){if(!ec) tick();});
}
void Bridge::stop() {
    stopping_=true;revoke("shutdown");
    const auto copy=clients_;for(const auto& c:copy) c.second.peer->close("Server shutdown");
}
}
