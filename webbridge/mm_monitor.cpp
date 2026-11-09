#include "mm_monitor.hpp"
#include <boost/beast.hpp>
#include <array>
#include <chrono>

namespace bridge {
namespace net=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
namespace {
std::string display_text(std::string text,bool bulletin) {
    // Bound WS/UI work independently of the persistent decoder archive.
    if(!bulletin) {
        size_t lines=0,pos=text.size();
        if(pos && text[pos-1]=='\n') --pos;
        while(pos) if(text[--pos]=='\n' && ++lines==200) {text.erase(0,pos+1);break;}
    }
    constexpr size_t maximum=65536;
    if(text.size()>maximum) {
        size_t cut=bulletin?maximum:text.size()-maximum;
        if(bulletin) {
            while(cut && (static_cast<unsigned char>(text[cut])&0xc0)==0x80) --cut;
            text.resize(cut);text+="\n[…]\n";
        } else {
            while(cut<text.size() && (static_cast<unsigned char>(text[cut])&0xc0)==0x80) ++cut;
            text.erase(0,cut);
        }
    }
    return text;
}
class Monitor final: public std::enable_shared_from_this<Monitor> {
    beast::tcp_stream socket_;
    net::steady_timer timer_;
    beast::flat_buffer buffer_;
    http::request<http::empty_body> request_;
    std::unique_ptr<http::response_parser<http::string_body>> response_;
    std::function<void(const std::string&,const std::string&)> receive_;
    const std::array<std::string,3> channels_{"dx-cluster","cw-skimmer","bulletin"};
    std::array<std::string,3> previous_;
    unsigned port_,channel_=0;
    bool stopped_=false,diagnostics_=false;
    void next() {
        boost::system::error_code ignored;socket_.socket().close(ignored);
        if(stopped_) return;
        channel_=diagnostics_?0:(channel_+1)%channels_.size();
        timer_.expires_after(std::chrono::milliseconds(diagnostics_?250:channel_==0?1000:0));
        timer_.async_wait([self=shared_from_this()](auto ec){if(!ec) self->fetch();});
    }
    void fetch() {
        if(stopped_) return;
        buffer_.consume(buffer_.size());
        response_=std::make_unique<http::response_parser<http::string_body>>();
        response_->body_limit(diagnostics_?131072:4*1024*1024+65536);
        request_={http::verb::get,diagnostics_?"/v1/diagnostics.json":"/v1/"+channels_[channel_]+".txt",11};
        request_.set(http::field::host,"127.0.0.1");request_.keep_alive(false);
        socket_.expires_after(std::chrono::seconds(2));
        socket_.async_connect({net::ip::make_address_v4("127.0.0.1"),static_cast<unsigned short>(port_)},
            [self=shared_from_this()](auto ec) {
                if(ec || self->stopped_) {self->next();return;}
                http::async_write(self->socket_,self->request_,[self](auto ec,size_t) {
                    if(ec || self->stopped_) {self->next();return;}
                    http::async_read(self->socket_,self->buffer_,*self->response_,[self](auto ec,size_t) {
                        if(!ec && !self->stopped_ && self->response_->get().result()==http::status::ok) {
                            auto text=std::move(self->response_->get().body());
                            if(!self->diagnostics_) text=display_text(std::move(text),self->channel_==2);
                            if(self->diagnostics_ || text!=self->previous_[self->channel_]) {
                                self->previous_[self->channel_]=text;
                                self->receive_(self->channels_[self->channel_],text);
                            }
                        }
                        self->next();
                    });
                });
            });
    }
public:
    Monitor(net::io_context& io,unsigned port,decltype(receive_) receive,bool diagnostics=false)
        :socket_(io),timer_(io),receive_(std::move(receive)),port_(port),diagnostics_(diagnostics) {}
    void start() {fetch();}
    void stop() {
        stopped_=true;timer_.cancel();boost::system::error_code ignored;socket_.socket().close(ignored);
    }
};
class Monitors final: public MmMonitor {
    net::io_context& io_;
    unsigned port_;
    std::shared_ptr<Monitor> text_,diagnostics_;
    std::function<void(const std::string&)> receive_diagnostics_;
public:
    Monitors(net::io_context& io,unsigned port,
            std::function<void(const std::string&,const std::string&)> text,
            std::function<void(const std::string&)> diagnostics)
        :io_(io),port_(port),text_(std::make_shared<Monitor>(io,port,std::move(text))),
         receive_diagnostics_(std::move(diagnostics)) {text_->start();}
    void diagnostics(bool enabled) override {
        if(enabled && !diagnostics_) {
            diagnostics_=std::make_shared<Monitor>(io_,port_,
                [receive=receive_diagnostics_](const auto&,const auto& body){receive(body);},true);
            diagnostics_->start();
        } else if(!enabled && diagnostics_) {diagnostics_->stop();diagnostics_.reset();}
    }
    void stop() override {text_->stop();diagnostics(false);}
};
}
std::shared_ptr<MmMonitor> monitor_mm(net::io_context& io,unsigned port,
        std::function<void(const std::string&,const std::string&)> receive,
        std::function<void(const std::string&)> diagnostics) {
    return std::make_shared<Monitors>(io,port,std::move(receive),std::move(diagnostics));
}
}
