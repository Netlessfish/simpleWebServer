#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>
#include "lst_timer.h"

#define FD_LIMIT 65535
#define MAX_EVENT_NUMBER 1024
#define TIMESLOT 5

/*
清理不活跃连接思路：
设置SIGALRM信号函数，隔5s发送一个SIGALRM信号
捕捉到SIGALRM信号后，将信号SIGALRM写入管道pipefd[1]
epoll_wait就会由于pipefd[0]可以读而被触发，进而设置timeout==true
利用timeout为真，调用timer_handler()，
在timer_handler()函数中，先调用tick()删除非活跃连接，然后再设置一个新的定时器alarm(TIMESLOT)，
新的定时器在TIMESLOT（5s）后，继续发送一个新的SIGALRM信号，
以此重复，即达到每隔IMESLOT（5s）后，清理一次非活跃连接
*/
static int pipefd[2];
static sort_timer_lst timer_lst;
static int epollfd = 0;

//修改文件属性为非阻塞
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

//捕捉到信号以后的处理函数
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );//把信号发送到了管道中，写入pipefd[1]
    errno = save_errno;
}

void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;//捕捉到信号以后的处理函数
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

//再次触发SIGALARM信号
void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( client_data* user_data )
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0 );
    assert( user_data );
    close( user_data->sockfd );
    printf( "close fd %d\n", user_data->sockfd );
}

int main( int argc, char* argv[] ) {
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename( argv[0] ) );
        return 1;
    }
    int port = atoi( argv[1] );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons( port );

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );

    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd );

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );//将管道写端设置为非阻塞
    addfd( epollfd, pipefd[0] );//监听管道读端

    // 设置信号处理函数
    addsig( SIGALRM );
    addsig( SIGTERM );
    bool stop_server = false;

    client_data* users = new client_data[FD_LIMIT]; 
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }
    
        for ( int i = 0; i < number; i++ ) {
            int sockfd = events[i].data.fd;
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                addfd( epollfd, connfd );
                users[connfd].address = client_address;
                users[connfd].sockfd = connfd;
                
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );//当前时间
                timer->expire = cur + 3 * TIMESLOT;//超时时间是 当前时间+15s以后
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );//把当前定时器添加进链表中
            } else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if(  events[i].events & EPOLLIN )
            {
                memset( users[sockfd].buf, '\0', BUFFER_SIZE );
                ret = recv( sockfd, users[sockfd].buf, BUFFER_SIZE-1, 0 );
                printf( "get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd );
                util_timer* timer = users[sockfd].timer;
                if( ret < 0 )
                {
                    // 如果发生读错误，则关闭连接，并移除其对应的定时器
                    if( errno != EAGAIN )
                    {
                        cb_func( &users[sockfd] );
                        if( timer )
                        {
                            timer_lst.del_timer( timer );
                        }
                    }
                }
                else if( ret == 0 )
                {
                    // 如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器。
                    cb_func( &users[sockfd] );
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                }
                else
                {
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;//延迟被关闭的时间
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                }
            }
           
        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        //当触发了一次SIGALRM信号后，timeout==true，然后就调用一次timer_handler()，再次触发触发 SIGALARM信号
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    return 0;
}