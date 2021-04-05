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
#include "lst_timer.h"

#define MAX_FD 65535//最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000//监听的最大的事件数量
#define TIMESLOT 5//定时器间隔时间

/*
清理不活跃连接思路：
设置SIGALRM信号函数，隔5s发送一个SIGALRM信号
捕捉到SIGALRM信号后，在sig_hanler()将信号SIGALRM写入管道pipefd[1]
epoll_wait就会由于pipefd[0]可以读而被触发，进而设置timeout==true
利用timeout为真，调用timer_handler()，
在timer_handler()函数中，先调用tick()删除非活跃连接，然后再设置一个新的定时器alarm(TIMESLOT)，
新的定时器在TIMESLOT（5s）后，继续发送一个新的SIGALRM信号，
以此重复，即达到每隔IMESLOT（5s）后，清理一次非活跃连接
*/
static int pipefd[2];
static sort_timer_lst timer_lst;

//捕捉到信号以后的处理函数
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );//把信号发送到了管道中，写入pipefd[1]
    errno = save_errno;
}

//再次触发SIGALARM信号
void timer_handler(int epollfd)
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick(epollfd);
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func( http_conn* user_data , int epollfd)
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, user_data->getSockfd(), 0 );
    assert( user_data );
    user_data->close_conn();
    printf( "close fd %d\n", user_data->getSockfd() );
}

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

// 设置文件描述符非阻塞
extern int setnonblocking( int fd );

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

    // 创建管道
    int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );//将管道写端设置为非阻塞
    addfd( epollfd, pipefd[0], false);//监听管道读端

    // 设置信号处理函数
    addsig( SIGALRM, sig_handler);
    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

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

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time( NULL );//当前时间
                timer->expire = cur + 3 * TIMESLOT;//超时时间是 当前时间+15s以后
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );//把当前定时器添加进链表中


            //普通的通信文件描述符有变化，有数据到达
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {
                
                util_timer* timer = users[sockfd].timer;
                //( EPOLLRDHUP | EPOLLHUP | EPOLLERR )这几个事件表示 对方异常断开或者错误等事件发生
                users[sockfd].close_conn();
                //关闭连接，并删除对应的定时器
                if(timer){
                    timer_lst.del_timer(timer);
                }
/*
当epollfd检测到读事件请求EPOLLIN，主线程一次性将数据读取完毕，
然后利用线程池将任务追加到线程池中，
线程池中run(),run()中从任务队列中取，取到一个后执行process()
process中将读取的数据，解析，生成响应数据。
响应有两块数据：一块是响应头和空行，一块是要请求的文件数据。用分散写的方法写两块数据。

下一次，当epollfd检测到可以写的时候，检测到EPOLLOUT时，就把响应数据写出去,调用write()
*/
            }else if( (sockfd == pipefd[0] ) && (events[i].events & EPOLLIN) ) {

                // 处理信号
                int sig;
                char signals[2048];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        if(signals[i] == SIGALRM){
                            // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                            // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                             timeout = true;
                             break;
                        }
                    }
                }
            }else if(events[i].events & EPOLLIN) { //判断有读的事件发生
                util_timer* timer = users[sockfd].timer;
                if(users[sockfd].read()) {
                    //一次性把所有数据都读完
                    pool->append(users + sockfd);
                    
                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;//延迟被关闭的时间
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }

                } else {//如果读取错误，关闭连接，并删除对应的定时器
                    users[sockfd].close_conn();
                    if(timer){
                         timer_lst.del_timer(timer);
                    }
                }

            }  else if( events[i].events & EPOLLOUT ) {//判断有写的事件发生

                if( !users[sockfd].write() ) {//一次性写完所有数据
                    users[sockfd].close_conn();
                }

            }
        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        //当触发了一次SIGALRM信号后，timeout==true，然后就调用一次timer_handler()，再次触发触发 SIGALARM信号
        if( timeout ) {
            timer_handler(epollfd);
            timeout = false;
        }

    }
    
    close( epollfd );
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;

    return 0;
}




