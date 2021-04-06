#ifndef EVENTPOLL
#define EVENTPOLL
#include "requestData.h"

const int MAXEVENTS = 5000;
const int LISTENQ = 10000;  //监听的最大事件数量

int epoll_init();
int epoll_add(int epoll_fd, int fd, void *request, __uint32_t events);//添加文件描述符到epoll中
int epoll_mod(int epoll_fd, int fd, void *request, __uint32_t events);//修改文件描述符属性，重置socket上的EPOLLINESHOT事件，以保证下一次可读时，EPOLLIN事件能被触发
int epoll_del(int epoll_fd, int fd, void *request, __uint32_t events);//从epoll中删除文件描述符
int my_epoll_wait(int epoll_fd, struct epoll_event *events, int max_events, int timeout);

#endif