#include <assert.h>
#include <sys/epoll.h>
#include <functional>
#include <iostream>
#include <string.h>
#include "Client.h"
#include "lib/Util.h"
#include "lib/EventLoop.h"
#include "lib/EventLoopThreadPool.h"
#include "lib/Channel.h"
#include "lib/CtlConn.h"
#include "lib/ProxyConn.h"
#include "Tunnel.h"
#include "LocalConn.h"


Client::Client(int workThreadNum, std::string proxy_server_host, u_int32_t proxy_server_port, std::string local_server_host, u_int32_t local_server_port) 
: loop_(new EventLoop()),
  eventLoopThreadPool_(new EventLoopThreadPool(workThreadNum)),
  proxy_server_host(proxy_server_host),
  proxy_server_port(proxy_server_port),
  local_server_host(local_server_host),
  local_server_port(local_server_port) {
    ignoreSigpipe();
  }

void Client::start() {
  eventLoopThreadPool_->start();
  initCtlConn();
  reqNewCtl();
  loop_->loop();
}

// 因为一个客户端只会有一个ctlConn，所以在ctlConn中的处理函数都不需要加锁处理
void Client::initCtlConn() {
  int conn_fd = tcp_connect(proxy_server_host.c_str(), proxy_server_port);
  assert(conn_fd > 0);
  SP_CtlConn conn(new CtlConn(conn_fd, loop_));
  ctl_conn_ = conn;
  ctl_conn_->setNewCtlRspHandler(
    std::bind(&Client::handleNewCtlRsp, this, std::placeholders::_1, std::placeholders::_2));
  ctl_conn_->setCloseHandler_(std::bind(&Client::handleCtlConnClose, this, std::placeholders::_1));
  ctl_conn_->setNewTunnelRspHandler(std::bind(&Client::handleNewTunnelRsp, this, std::placeholders::_1, std::placeholders::_2));
  ctl_conn_->setNotifyClientNeedProxyHandler(std::bind(&Client::handleProxyNotify, this, std::placeholders::_1, std::placeholders::_2));
  ctl_conn_->setNotifyProxyShutdownPeerConnHandler_(std::bind(&Client::handleShutdownLocalConn, this, std::placeholders::_1, std::placeholders::_2));
  loop_->addToPoller(ctl_conn_->getChannel());
}
void Client::handleNewCtlRsp(void* msg, SP_CtlConn conn) {
  NewCtlRspMsg *new_ctl_rsp_msg = (NewCtlRspMsg *)msg;
  conn->set_ctl_id(std::string(new_ctl_rsp_msg->ctl_id));
  client_id = std::string(new_ctl_rsp_msg->ctl_id);
  reqNewTunnel();
}

void Client::handleNewTunnelRsp(void *msg, SP_CtlConn conn) {
  NewTunnelRspMsg *rsp_msg = (NewTunnelRspMsg *)msg;
  std::string tun_id = std::string(rsp_msg->tun_id);
  std::string local_server_host = std::string(rsp_msg->local_server_host);
  std::string proxy_server_host = std::string(rsp_msg->proxy_server_host);
  SP_Tunnel tun(new Tunnel{tun_id, local_server_host, rsp_msg->local_server_port, 
  proxy_server_host, rsp_msg->proxy_server_port, this, eventLoopThreadPool_});
  printf("tunnel addr:%s:%d\n", rsp_msg->proxy_server_host, rsp_msg->proxy_server_port);
  tunnel_map_.emplace(rsp_msg->tun_id, tun);
}

// 请求新ctl
void Client::reqNewCtl() {
  // 请求新ctl
  NewCtlReqMsg req_msg = NewCtlReqMsg{};
  CtlMsg msg = make_ctl_msg(NewCtlReq, (char *)(&req_msg), sizeof(req_msg));
  ctl_conn_->send_msg(msg);
};

// 新建应用隧道
void Client::reqNewTunnel() {
  NewTunnelReqMsg req_msg;
  req_msg.local_server_port = local_server_port;
  strcpy(req_msg.local_server_host, local_server_host.c_str());
  CtlMsg msg = make_ctl_msg(NewTunnelReq, (char *)(&req_msg), sizeof(req_msg));
  ctl_conn_->send_msg(msg);
}

// 处理服务端通知创建proxyConn
void Client::handleProxyNotify(void *msg, SP_CtlConn conn) {
  NotifyClientNeedProxyMsg *req_msg = (NotifyClientNeedProxyMsg*)msg;
  std::string tun_id = req_msg->tun_id;
  // 检查tun_id是否存在
  if (tunnel_map_.find(tun_id) == tunnel_map_.end()) {
    printf("tun_id %s not exist\n", tun_id.c_str());
    return;
  }

  SP_Tunnel tun = tunnel_map_[tun_id];
  // 创建proxyConn
  SP_ProxyConn proxyConn = tun->createProxyConn(req_msg->server_proxy_port);
  (tun->proxy_conn_map).add(proxyConn->getProxyID(), proxyConn);

  // 创建LocalConn
  SP_LocalConn localConn = tun->createLocalConn(proxyConn->getProxyID());

  // proxy设置为开始
  proxyConn->start(localConn);

  // 发送给服务端告知这个代理链接一些元信息
  ProxyMetaSetMsg meta_set_req_msg = ProxyMetaSetMsg{};
  strcpy(meta_set_req_msg.ctl_id, client_id.c_str());
  strcpy(meta_set_req_msg.tun_id, tun_id.c_str());
  strcpy(meta_set_req_msg.proxy_id, (proxyConn->getProxyID()).c_str());
  ProxyCtlMsg proxy_ctl_msg = make_proxy_ctl_msg(ProxyMetaSet, (char *)&meta_set_req_msg, sizeof(meta_set_req_msg));
  proxyConn->send_msg(proxy_ctl_msg);
}

// 处理关闭localConn
void Client::handleShutdownLocalConn(void *msg, SP_CtlConn conn) {
  NotifyProxyShutdownPeerConnMsg *req_msg = (NotifyProxyShutdownPeerConnMsg *)msg;
  std::string tun_id = req_msg->tun_id;
  std::string proxy_id = req_msg->proxy_id;
  // 检查tun_id是否存在
  if (tunnel_map_.find(tun_id) == tunnel_map_.end()) {
    printf("tun_id %s not exist\n", tun_id.c_str());
    return;
  }
  SP_Tunnel tun = tunnel_map_[tun_id];
  bool isProxyExist;
  SP_ProxyConn proxyConn = (tun->proxy_conn_map).get(proxy_id, isProxyExist);
  if (!isProxyExist) {
    printf("[handleShutdownLocalConn] proxy_id: %s not exist\n", proxy_id.c_str());
    return;
  }
  bool isFree = proxyConn->shutdownFromRemote();
  // 如果本端代理连接已经空闲，需要通知将此代理释放到空闲列表中
  if (isFree) {
    FreeProxyConnReqMsg req_msg;
    strcpy(req_msg.tun_id, tun_id.c_str());
    strcpy(req_msg.proxy_id, proxy_id.c_str());
    CtlMsg ctl_msg = make_ctl_msg(FreeProxyConnReq, (char *)&req_msg, sizeof(FreeProxyConnReqMsg));
    ctl_conn_->send_msg(ctl_msg);
  }
  printf("[%s][handleShutdownLocalConn] proxy_id: %s, isFree: %d\n", getNowTime(),proxy_id.c_str(), isFree);
};

void Client::shutdownFromLocal(std::string tun_id, std::string proxy_id) {
  NotifyProxyShutdownPeerConnMsg req_msg;
  strcpy(req_msg.tun_id, tun_id.c_str());
  strcpy(req_msg.proxy_id, proxy_id.c_str());
  CtlMsg ctl_msg = make_ctl_msg(NotifyProxyShutdownPeerConn, (char *)&req_msg, sizeof(NotifyProxyShutdownPeerConnMsg));
  ctl_conn_->send_msg(ctl_msg);
};