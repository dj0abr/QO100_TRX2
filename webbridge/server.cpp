#include "bridge.hpp"
#include <boost/beast.hpp>
#include <deque>
#include <filesystem>
#include <iostream>

namespace bridge {
namespace beast=boost::beast;
namespace http=beast::http;
namespace websocket=beast::websocket;
using Tcp=net::ip::tcp;
class WebSocket final: public Peer,public std::enable_shared_from_this<WebSocket> {
    struct Out {std::shared_ptr<const Bytes> bytes;bool binary;Clock::time_point time=Clock::now();};
    websocket::stream<beast::tcp_stream> ws_;
    beast::flat_buffer incoming_;
    Bridge& bridge_;
    std::deque<Out> queue_;
    size_t queued_=0;
    uint64_t id_=0;
    bool closed_=false,closing_=false;
    std::string reason_;
    void abort_later() {
        if(closed_ || closing_) return;
        closing_=true;
        // Do not re-enter Bridge::leave() from inside a bridge broadcast/control.
        net::post(ws_.get_executor(),[self=shared_from_this()]{self->finish();});
    }
    void finish() {
        if(closed_) return;
        closed_=true;boost::system::error_code ec;
        beast::get_lowest_layer(ws_).socket().shutdown(Tcp::socket::shutdown_both,ec);
        beast::get_lowest_layer(ws_).socket().close(ec);
        if(id_) {const auto id=id_;id_=0;bridge_.leave(id);}
        queue_.clear();queued_=0;
    }
    void read() {
        ws_.async_read(incoming_,[self=shared_from_this()](auto ec,size_t) {
            if(ec) {self->finish();return;}
            const auto buffers=self->incoming_.data();Bytes bytes(self->incoming_.size());
            net::buffer_copy(net::buffer(bytes),buffers);
            self->bridge_.message(self->id_,bytes,!self->ws_.got_text());
            self->incoming_.consume(self->incoming_.size());
            if(!self->closed_ && !self->closing_) self->read();
        });
    }
    void write() {
        if(closed_) return;
        if(queue_.empty()) {
            if(closing_) {
                websocket::close_reason reason(websocket::close_code::normal);reason.reason=reason_.substr(0,100);
                ws_.async_close(reason,[self=shared_from_this()](auto){self->finish();});
            }
            return;
        }
        const auto out=queue_.front();ws_.binary(out.binary);
        ws_.async_write(net::buffer(*out.bytes),[self=shared_from_this(),keep=out.bytes](auto ec,size_t) {
            if(ec || self->closed_) {self->finish();return;}
            self->queued_-=self->queue_.front().bytes->size();self->queue_.pop_front();self->write();
        });
    }
public:
    WebSocket(beast::tcp_stream stream,Bridge& bridge):ws_(std::move(stream)),bridge_(bridge) {}
    void accept(http::request<http::string_body> request) {
        beast::get_lowest_layer(ws_).expires_never();
        ws_.set_option(websocket::stream_base::timeout{std::chrono::seconds(10),std::chrono::seconds(15),true});
        ws_.read_message_max(8192);
        auto keep=std::make_shared<http::request<http::string_body>>(std::move(request));
        ws_.async_accept(*keep,[self=shared_from_this(),keep](auto ec) {
            if(ec) {self->finish();return;}
            boost::system::error_code ignored;
            beast::get_lowest_layer(self->ws_).socket().set_option(Tcp::no_delay(true),ignored);
            beast::get_lowest_layer(self->ws_).socket().set_option(net::socket_base::send_buffer_size(65536),ignored);
            self->id_=self->bridge_.join(self);
            if(self->id_) self->read();
        });
    }
    void send(std::shared_ptr<const Bytes> bytes,bool binary) override {
        if(closed_ || closing_) return;
        const auto limit=bridge_.config().queue_bytes;
        if(!queue_.empty() && Clock::now()-queue_.front().time>std::chrono::milliseconds(750)) {abort_later();return;}
        if(binary && bytes->size()>=6 && (*bytes)[5]==2 && queued_>limit/2) {
            // Keep the active async_write buffer, discard only queued FFT fragments.
            for(auto it=std::next(queue_.begin());it!=queue_.end();) {
                if(it->binary && it->bytes->size()>=6 && (*it->bytes)[5]==2) {
                    queued_-=it->bytes->size();it=queue_.erase(it);
                } else ++it;
            }
        }
        if(queued_+bytes->size()>limit) {abort_later();return;}
        const bool idle=queue_.empty();queued_+=bytes->size();queue_.push_back({std::move(bytes),binary});
        if(idle) write();
    }
    void close(const std::string& reason) override {
        if(closed_ || closing_) return;
        closing_=true;reason_=reason;
        // Do not leave old audio/FFT ahead of the close notification.
        if(!queue_.empty()) {
            for(auto it=std::next(queue_.begin());it!=queue_.end();) {
                if(it->binary) {queued_-=it->bytes->size();it=queue_.erase(it);} else ++it;
            }
        } else write();
    }
};
class Http final: public std::enable_shared_from_this<Http> {
    beast::tcp_stream stream_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    Bridge& bridge_;
    template<class Body> void reply(http::response<Body> response) {
        response.set(http::field::server,"QO100-WebBridge");response.set(http::field::cache_control,"no-store");
        response.set("X-Content-Type-Options","nosniff");response.keep_alive(false);
        auto shared=std::make_shared<http::response<Body>>(std::move(response));
        http::async_write(stream_,*shared,[self=shared_from_this(),shared](auto,size_t) {
            boost::system::error_code ignored;self->stream_.socket().shutdown(Tcp::socket::shutdown_send,ignored);
        });
    }
    void error(http::status status,const std::string& message) {
        http::response<http::string_body> response{status,11};response.set(http::field::content_type,"text/plain; charset=utf-8");
        response.body()=message;response.prepare_payload();reply(std::move(response));
    }
    void handle() {
        auto request=parser_.release();
        if(request.method()!=http::verb::get && request.method()!=http::verb::head) {error(http::status::method_not_allowed,"GET or HEAD required");return;}
        auto target=std::string(request.target());target=target.substr(0,target.find('?'));
        if(target=="/ws" && websocket::is_upgrade(request)) {
            const auto origin=request[http::field::origin];
            if(!origin.empty()) {
                const std::string s(origin);const auto scheme=s.find("://");
                if(scheme==std::string::npos || s.substr(scheme+3)!=std::string(request[http::field::host])) {
                    error(http::status::forbidden,"Origin must match Host");return;
                }
            }
            std::make_shared<WebSocket>(std::move(stream_),bridge_)->accept(std::move(request));return;
        }
        if(target=="/ws") {error(http::status::upgrade_required,"WebSocket upgrade required");return;}
        if(target=="/health") {
            http::response<http::string_body> response{http::status::ok,11};response.set(http::field::content_type,"application/json");
            response.body()="{\"service\":\"qo100_webbridge\"}";response.prepare_payload();reply(std::move(response));return;
        }
        // Static assets only, constrained to the configured document root, including symlinks.
        if(target.empty() || target[0]!='/' || target.find_first_of("%\\\0",0,3)!=std::string::npos) {
            error(http::status::bad_request,"Invalid path");return;
        }
        if(target=="/") target="/index.html";
        std::error_code ec;const auto path=std::filesystem::canonical(std::filesystem::path(bridge_.config().web_root)/target.substr(1),ec);
        const auto root=bridge_.config().web_root+"/";
        if(ec || path.string().compare(0,root.size(),root)!=0 || !std::filesystem::is_regular_file(path,ec)) {
            error(http::status::not_found,"Not found");return;
        }
        const auto ext=path.extension().string();std::string mime="application/octet-stream";
        if(ext==".html") mime="text/html; charset=utf-8";else if(ext==".js") mime="text/javascript; charset=utf-8";
        else if(ext==".css") mime="text/css; charset=utf-8";else if(ext==".svg") mime="image/svg+xml";
        else if(ext==".json") mime="application/json";else if(ext==".png") mime="image/png";
        if(request.method()==http::verb::head) {
            http::response<http::empty_body> response{http::status::ok,11};response.set(http::field::content_type,mime);
            response.content_length(std::filesystem::file_size(path));reply(std::move(response));return;
        }
        http::file_body::value_type file;boost::system::error_code err;file.open(path.c_str(),beast::file_mode::scan,err);
        if(err) {error(http::status::not_found,"Not found");return;}
        http::response<http::file_body> response{std::piecewise_construct,std::make_tuple(std::move(file)),std::make_tuple(http::status::ok,11)};
        response.set(http::field::content_type,mime);response.content_length(response.body().size());reply(std::move(response));
    }
public:
    Http(Tcp::socket socket,Bridge& bridge):stream_(std::move(socket)),bridge_(bridge) {parser_.body_limit(8192);parser_.header_limit(8192);}
    void start() {
        stream_.expires_after(std::chrono::seconds(10));
        http::async_read(stream_,buffer_,parser_,[self=shared_from_this()](auto ec,size_t){if(!ec) self->handle();});
    }
};
class Listener final: public std::enable_shared_from_this<Listener> {
    Tcp::acceptor acceptor_;
    Bridge& bridge_;
    void accept() {
        acceptor_.async_accept([self=shared_from_this()](auto ec,Tcp::socket socket) {
            if(!ec) std::make_shared<Http>(std::move(socket),self->bridge_)->start();
            if(ec!=net::error::operation_aborted) self->accept();
        });
    }
public:
    Listener(net::io_context& io,Bridge& bridge):acceptor_(io),bridge_(bridge) {
        const Tcp::endpoint endpoint{net::ip::make_address_v4(bridge.config().listen),bridge.config().port};
        acceptor_.open(endpoint.protocol());acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);acceptor_.listen(net::socket_base::max_listen_connections);
    }
    void start() {accept();}
};
void listen(net::io_context& io,Bridge& bridge) {std::make_shared<Listener>(io,bridge)->start();}
}
