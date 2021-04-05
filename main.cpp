#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"

#define MAX_FD 65535//最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000//监听的最大的事件数量

/*
我们的任务是模拟proactor
主线程监听连接事件，
当有连接事件时，在主线程中把数据一次性读取出来，封装成一个任务类（http_conn），交给子线程（工作线程），插入到线程池中
然后子线程从线程池中取任务，执行任务
*/

//添加信号捕捉
void addsig(int sig, void(handler)(int) ){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));//清空sa
    sa.sa_handler = handler;//捕捉到信号以后的处理函数
    sigfillset(&sa.sa_mask);//设置临时的阻塞信号集
    sigaction(sig, &sa, NULL);//注册某个信号sig捕捉

}

//添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);

//从epollfd中删除文件描述符
extern void removefd(int epollfd, int fd);

//修改文件描述符
//extern void modfd(int epollfd, int fd，int ev);

int main(int arg, char * argv[]){
    if(arg <= 1){
        //至少要传递一个端口号
        printf("按照如下格式运行： %s port_number\n",basename(argv[0]));
        exit(-1);
    }

    //获取端口号
    int port = atoi(argv[1]);//从字符串转换为int型,argv[0]是程序名
    
    //对SIGPIPE信号(终止进程)进行处理
    addsig(SIGPIPE, SIG_IGN);//捕捉到这个信号后忽略

    //创建线程池，初始化线程池
    //线程池中的任务模板T：接收客户端发送http连接
    threadpool<http_conn> *pool = NULL; 
    try{
        pool = new threadpool<http_conn>;
    }catch(...){//捕捉到异常退出
        exit(-1);
    }

    //创建一个数组保存所有的客户端信息
    http_conn * users = new http_conn[ MAX_FD];

    //创建监听的套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    //设置端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //绑定
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;//服务端指定任意一个地址
    address.sin_family = AF_INET;
    address.sin_port = htons( port );//将主机字节序转化为网络字节序
    bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    
    //监听
    listen( listenfd, 5 );

    //创建epoll对象，事件数组，添加监听的文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);

    //将监听的文件描述符添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    //主线程不断地while循环检测是否有哪些事件发生
    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        //调用epoll失败
        //如果程序被某个信号中断，然后会返回-1，这种情况下需要对EINTR进行判断，不能直接退出;
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        //循环遍历事件数组
        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;//获取监听到的文件描述符
            
            if( sockfd == listenfd ) {//监听的文件描述符有变化，表示有客户端连接进来
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );//接收新客户端的文件描述符
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    //目前连接数满了
                    //给客户端写一个信息：服务器内部正忙
                    close(connfd);
                    continue;
                }

                //将新的客户的数据初始化，放到数组中
                //以文件描述符connfd作为数组索引，因为新的connfd也是+1递增的
                users[connfd].init( connfd, client_address);

            //普通的通信文件描述符有变化，有数据到达
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                
                //( EPOLLRDHUP | EPOLLHUP | EPOLLERR )这几个事件表示 对方异常断开或者错误等事件发生
                users[sockfd].close_conn();
/*
当epollfd检测到读事件请求EPOLLIN，主线程一次性将数据读取完毕，
然后利用线程池将任务追加到线程池中，
线程池中run(),run()中从任务队列中取，取到一个后执行process()
process中将读取的数据，解析，生成响应数据。
响应有两块数据：一块是响应头和空行，一块是要请求的文件数据。用分散写的方法写两块数据。

下一次，当epollfd检测到可以写的时候，检测到EPOLLOUT时，就把响应数据写出去,调用write()
*/
            } else if(events[i].events & EPOLLIN) { //判断有读的事件发生

                if(users[sockfd].read()) {
                    //一次性把所有数据都读完
                    pool->append(users + sockfd);
                } else {
                    users[sockfd].close_conn();
                }

            }  else if( events[i].events & EPOLLOUT ) {//判断有写的事件发生

                if( !users[sockfd].write() ) {//一次性写完所有数据
                    users[sockfd].close_conn();
                }

            }
        }
    }
    
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;

    return 0;
}




