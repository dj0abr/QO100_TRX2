#pragma once
#include <boost/asio.hpp>
#include <functional>
#include <memory>
#include <string>

namespace bridge {
class MmMonitor {
public:
    virtual ~MmMonitor()=default;
    virtual void stop()=0;
    virtual void diagnostics(bool enabled)=0;
};
// Nonblocking HTTP snapshots from the separately supervised headless receiver.
std::shared_ptr<MmMonitor> monitor_mm(boost::asio::io_context& io,unsigned port,
    std::function<void(const std::string&,const std::string&)> receive,
    std::function<void(const std::string&)> diagnostics);
}
