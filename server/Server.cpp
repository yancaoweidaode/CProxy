#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <exception>
#include <iostream>
#include <functional>

#include "Server.h"
#include "lib/Util.h"
#include "lib/EventLoopThreadPool.h"
#include "lib/EventLoopThread.h"
#include "lib/CtlConn.h"
#include "lib/Msg.h"
#include "lib/ProxyConn.h"

const int SERVER_LISTEN_EPOLL_EVENTS = (EPOLLIN | EPOLLET | EPOLLRDHUP);

Server::Server(int threadNum, int ctlPort, int proxyPort)
  : threadNum_(threadNum),
    ctlPort_(ctlPort),
    proxyPort_(proxyPort),
    ctlListenFd_(socketBindListen(ctlPort_)),
    proxyListenFd_(socketBindListen(proxyPort_)),
    loop_(new EventLoop()) {
      // ctl相关
      if (ctlListenFd_ < 0) {
        perror("listen socket fail");
        abort();
      }
      ignoreSigpipe();
      if (setfdNonBlock(ctlListenFd_) == -1) {
        perror("set non block fail");
        abort();
      }
      ctl_acceptor_ = SP_Channel(new Channel(ctlListenFd_));
      ctl_acceptor_->setEvents(SERVER_LISTEN_EPOLL_EVENTS);
      ctl_acceptor_->setReadHandler(std::bind(&Server::newCtlConnHandler, this));
      ctl_acceptor_->setPostHandler(std::bind(&Server::postHandler, this));
      loop_->addToPoller(ctl_acceptor_);
      
      // proxy相关
      if (proxyListenFd_ < 0) {
        perror("listen socket fail");
        abort();
      }
      if (setfdNonBlock(proxyListenFd_) == -1) {
        perror("set non block fail");
        abort();
      }
      proxy_acceptor_ = SP_Channel(new Channel(proxyListenFd_));
      proxy_acceptor_->setEvents(SERVER_LISTEN_EPOLL_EVENTS);
      proxy_acceptor_->setReadHandler(std::bind(&Server::newProxyConnHandler, this));
      proxy_acceptor_->setPostHandler(std::bind(&Server::postHandler, this));
      loop_->addToPoller(proxy_acceptor_);
      
      publicListenThread_ = SP_EventLoopThread(new EventLoopThread());
      eventLoopThreadPool_ = SP_EventLoopThreadPool(new EventLoopThreadPool(threadNum));

      // 初始化其他成员数据
      for (int i = 0; i < UnclaimedProxyMapLen; i ++) {
        hashedUnclaimedProxyMaps[i] = new UnclaimedProxyMap{};
      }
    }

void Server::start() 
try{
// 启动数据交换线程池
  eventLoopThreadPool_->start();
// 确保public连接监听的线程启动
  publicListenThread_->startLoop();
// 主epoll开始轮训
  loop_->loop();
}catch (const std::exception& e) {
  std::cout << e.what() << std::endl;
  abort();
}

// 新ctl连接
void Server::newCtlConnHandler() 
try {
  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(struct sockaddr_in));
  socklen_t client_addr_len = sizeof(client_addr);
  int accept_fd = 0;
  while((accept_fd = accept(ctlListenFd_, (struct sockaddr *)&client_addr, &client_addr_len)) > 0) {
    printf("ctl_accept_fd: %d; from:%s; port:%d", accept_fd, inet_ntoa(client_addr.sin_addr),ntohs(client_addr.sin_port));
    // 选定一个未使用的ctl_id
    std::string ctl_id = rand_str(5);
    while(control_map_.find(ctl_id) != control_map_.end()) {
      ctl_id = rand_str(5);
    }

    SP_Control ctl(new Control(accept_fd, ctl_id, loop_, this));
    control_map_.emplace(ctl_id, ctl);
  }
}
catch (const std::exception& e) {
  std::cout << "new ctl conn handler err: "<< e.what() << std::endl;
};

// 新proxy连接
void Server::newProxyConnHandler() 
try{
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  memset(&client_addr, 0, client_addr_len);
  int accept_fd = 0;
  while((accept_fd = accept(proxyListenFd_, (struct sockaddr *)&client_addr, &client_addr_len)) > 0 ) {
    printf("proxy_accept_fd: %d; from:%s; port:%d\n", accept_fd, inet_ntoa(client_addr.sin_addr),ntohs(client_addr.sin_port));
    // 封装ProxyConn,并选择一个工作线程处理
    SP_EventLoopThread threadPicked = eventLoopThreadPool_->pickRandThread();
    SP_ProxyConn proxyConn(new ProxyConn(accept_fd, threadPicked->getLoop()));
    proxyConn->setProxyMetaSetHandler(std::bind(&Server::claimProxyConn, this, std::placeholders::_1, std::placeholders::_2));

    // 选择一个未认领proxy的map
    UnclaimedProxyMap *unclaimedProxyMap = getUnclaimedProxyMapByFd(accept_fd);
    {
      std::unique_lock<std::mutex> lock(unclaimedProxyMap->mutex);
      (unclaimedProxyMap->conns).emplace(accept_fd, proxyConn);
    }
    threadPicked->addConn(proxyConn);
  }
}
catch (const std::exception& e) {
  std::cout << "new proxy conn handler err: "<< e.what() << std::endl;
}

void Server::postHandler() {
  ctl_acceptor_->setEvents(SERVER_LISTEN_EPOLL_EVENTS);
  loop_->updatePoller(ctl_acceptor_);
}

void Server::claimProxyConn(void *msg, SP_ProxyConn conn) {
  ProxyMetaSetMsg *proxy_meta_set_msg = (ProxyMetaSetMsg *)msg;
  std::string ctl_id = std::string(proxy_meta_set_msg->ctl_id);
  std::string tun_id = std::string(proxy_meta_set_msg->tun_id);
  std::string proxy_id = std::string(proxy_meta_set_msg->proxy_id);
  conn->setProxyID(proxy_id);
  if (control_map_.find(ctl_id) == control_map_.end()) {
    printf("control %s is not exist\n", ctl_id.c_str());
    return;
  }
  SP_Control ctl = control_map_[ctl_id];
  if (!(ctl->tunnel_map_).isExist(tun_id)) {
    printf("tun %s is not exist in ctl %s\n", tun_id.c_str(), ctl_id.c_str());
    return;
  }
  SP_Tunnel tun = (ctl->tunnel_map_).get(tun_id);
  tun->claimProxyConn(conn);
  // proxyConn已被认领，需从unclaimedProxyMaps中去掉
  int proxy_conn_fd = conn->getFd();
  UnclaimedProxyMap *unclaimedProxyMap = getUnclaimedProxyMapByFd(proxy_conn_fd);
  {
    std::unique_lock<std::mutex>lock(unclaimedProxyMap->mutex);
    (unclaimedProxyMap->conns).erase(proxy_conn_fd);
  }
}

// 根据fd获取对应所在的map
UnclaimedProxyMap* Server::getUnclaimedProxyMapByFd(int fd) {
  int idx = fd % UnclaimedProxyMapLen;
  return hashedUnclaimedProxyMaps[idx];
}