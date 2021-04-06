#ifndef THREADPOOL
#define THREADPOOL
#include "requestData.h"
#include <pthread.h>

const int THREADPOOL_INVALID = -1;         //线程池无效
const int THREADPOOL_LOCK_FAILURE = -2;    //加锁失败
const int THREADPOOL_QUEUE_FULL = -3;      //请求队列已满
const int THREADPOOL_SHUTDOWN = -4;        //关闭错误：重复关闭
const int THREADPOOL_THREAD_FAILURE = -5;  //线程资源回收失败
const int THREADPOOL_GRACEFUL = 1;

const int MAX_THREADS = 1024;   //线程池最大允许的线程数
const int MAX_QUEUE = 65535;    //请求队列最大数量

typedef enum 
{
    immediate_shutdown = 1,
    graceful_shutdown  = 2
} threadpool_shutdown_t;

typedef struct {
    void (*function)(void *);  //函数指针指向任务函数
    void *argument;            //function的参数
} threadpool_task_t;

struct threadpool_t
{
    /*---线程同步：互斥锁，条件变量--- */
    pthread_mutex_t lock;            //互斥锁
    pthread_cond_t notify;           //条件变量

    pthread_t *threads;              //线程队列对象 用数组去表达 数组中每一个元素代表一个线程id
    int thread_count;                //线程数目

    threadpool_task_t *queue;        //请求任务队列：数组中每一个元素代表一个任务对象
    int queue_size;                  //请求队列大小
    int head;                        //请求队列头索引（第一个任务）
    int tail;                        //请求队列尾索引（最后一个任务）
    int count;                       //挂在队列中任务数目
    int shutdown;                    //线程池关闭标志
    int started;                     //已经启动的线程数量
};

threadpool_t *threadpool_create(int thread_count, int queue_size, int flags);                //线程池创建并初始化        
int threadpool_add(threadpool_t *pool, void (*function)(void *), void *argument, int flags); //给线程池中添加任务
int threadpool_destroy(threadpool_t *pool, int flags);                                       //按照正常流程，摧毁线程池
int threadpool_free(threadpool_t *pool);                                                     //直接释放线程池
static void *threadpool_thread(void *threadpool);                                            //线程绑定函数：从请求队列中，取出第一个任务，并去执行                                        //从线程池中取出线程，去执行请求队列的第一个任务

#endif