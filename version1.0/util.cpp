#include "util.h"
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

//循环读取数据，直到把给定长度n的所有数据读完为止
ssize_t readn(int fd, void *buff, size_t n)
{
    size_t nleft = n;         //指定存放读取数据数组的大小
    ssize_t nread = 0;        //当前read中已经读取到的字节数
    ssize_t readSum = 0;      //最终全部读取到的总字节数
    char *ptr = (char*)buff;

    while (nleft > 0)
    {
        if ((nread = read(fd, ptr, nleft)) < 0) {
            if (errno == EINTR)
                nread = 0;
            else if (errno == EAGAIN){
                return readSum;
            }
            else{
                return -1;
            }  
        }
        else if (nread == 0)//read返回值为0，表示数据已经读取完了
            break;
        readSum += nread;   //将此次read到的字节数nread 更新 到全部读取到的总字节数readSum
        nleft -= nread;
        ptr += nread;       //更新存放数据的数组首地址
    }
    return readSum;
}

ssize_t writen(int fd, void *buff, size_t n)
{
    size_t nleft = n;
    ssize_t nwritten = 0;
    ssize_t writeSum = 0;
    char *ptr = (char*)buff;
    while (nleft > 0)
    {
        if ((nwritten = write(fd, ptr, nleft)) <= 0)
        {
            if (nwritten < 0)
            {
                if (errno == EINTR || errno == EAGAIN){//这两种情况不是真正的写入错误，需要单独判断重新写入
                    nwritten = 0;
                    continue;
                }
                else
                    return -1;
            }
        }
        writeSum += nwritten;
        nleft -= nwritten;
        ptr += nwritten;
    }
    return writeSum;
}

//处理sigpipe信号
void handle_for_sigpipe(){
    struct sigaction sa; //信号处理结构体
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = SIG_IGN;//设置信号的处理回调函数，SIG_IGN宏代表的操作就是忽略该信号 
    sa.sa_flags = 0;
    if(sigaction(SIGPIPE, &sa, NULL))//注册sigpipe的捕捉，将信号和信号的处理结构体绑定
        return;
}

//将文件描述符设置为非阻塞
int setSocketNonBlocking(int fd){
    int flag = fcntl(fd, F_GETFL, 0);
    if(flag == -1){
        return -1;
    }
    flag |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flag) == -1)
        return -1;
    return 0;
}