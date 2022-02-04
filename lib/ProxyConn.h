#pragma once

#include <memory>

#include "Conn.h"
#include "TranConn.h"
#include "EventLoop.h"
#include "Buffer.h"

const int MAX_MSG_LEN = 2048;

enum ProxyCtlMsgType {
  ProxyMetaSet
};

struct ProxyCtlMsg {
  u_int32_t len;
  ProxyCtlMsgType type;
  char data[MAX_MSG_LEN];
};

struct ProxyMetaSetMsg {
  char ctl_id[10];
  char tun_id[10];
};

class ProxyConn : public TranConn, public std::enable_shared_from_this<ProxyConn> {
  public:
    typedef std::shared_ptr<ProxyConn> SP_ProxyConn;
    typedef std::function<void(void*, SP_ProxyConn)> MsgHandler;
    ProxyConn(int fd, SP_EventLoop loop) 
    : TranConn{fd, loop},
      out_buffer_(new Buffer(1024, 65536)),
      is_start_(false) {
        channel_->setEvents(EPOLLET | EPOLLIN  | EPOLLOUT | EPOLLRDHUP);
        channel_->setReadHandler(std::bind(&ProxyConn::handleRead, this));
        channel_->setWriteHandler(std::bind(&ProxyConn::handleWrite, this));
        channel_->setPostHandler(std::bind(&ProxyConn::postHandle, this));
    }
    ~ProxyConn() {
      printf("proxyConn killing\n");
    }
    // server端需设置
    void setProxyMetaSetHandler(MsgHandler handler) {proxyMetaSetHandler_ = handler;}

    void send_msg(ProxyCtlMsg& msg);
    void start(SP_TranConn conn) {
      is_start_ = true;
      peerConn_ = conn;
      setPeerConnFd(conn->getFd());
      conn->setPeerConnFd(fd_);
    }
    
  private:
    SP_Buffer out_buffer_;
    SP_Conn peerConn_;
    bool is_start_;
    void handleRead();
    void handleWrite();
    void postHandle();
    MsgHandler proxyMetaSetHandler_ = [](void*, SP_ProxyConn) {};
};
ProxyCtlMsg make_proxy_ctl_msg(ProxyCtlMsgType type, char *data, size_t data_len);
size_t get_proxy_ctl_msg_body_size(const ProxyCtlMsg& msg);

using SP_ProxyConn = std::shared_ptr<ProxyConn>;