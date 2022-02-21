#pragma once

#include <memory>

#include "lib/TranConn.h"
#include "lib/EventLoop.h"

class Tunnel;
class LocalConn : public TranConn, public std::enable_shared_from_this<LocalConn>{
  public:
    LocalConn(int fd, SP_EventLoop loop, Tunnel* tun, std::string proxy_id)
    : TranConn(fd, loop), 
      tun_(tun), 
      proxy_id_(proxy_id),
      closing_(false) {
      channel_->setEvents(EPOLLET | EPOLLIN | EPOLLRDHUP);
      channel_->setReadHandler(std::bind(&LocalConn::handleRead, this));
      channel_->setPostHandler(std::bind(&LocalConn::postHandle, this));
    }
    ~LocalConn(){
      printf("local killing\n");
    }

  private:
    Tunnel* tun_;
    std::string proxy_id_;
    bool closing_;
    void handleRead();
    void postHandle();
};

using SP_LocalConn = std::shared_ptr<LocalConn>;