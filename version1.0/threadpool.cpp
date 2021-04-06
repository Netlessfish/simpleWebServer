#include "threadpool.h"

/* 线程池创建并初始化 */
threadpool_t *threadpool_create(int thread_count, int queue_size, int flags) {
    threadpool_t *pool=NULL;//线程池对象
    int i;
    do
    {
        // 先判断传入参数是否违法
        if(thread_count <= 0 || thread_count > MAX_THREADS || queue_size <= 0 || queue_size > MAX_QUEUE) {
            return NULL;
        }

        // 创建一个线程池对象
        if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL) {
            break;
        }
    
        /* 初始化线程池参数 */ 
        pool->thread_count = 0;                         //初始化线程数量
        pool->queue_size = queue_size;                  //初始化请求队列大小
        pool->head = pool->tail = pool->count = 0;      //初始化请求队列头,尾，队列中任务数量
        pool->shutdown = pool->started = 0;             //初始化启动和关闭标志为0
        pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);              //初始化线程池中线程队列
        pool->queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t) * queue_size);  //初始化线程池中任务队列 
    
        if((pthread_mutex_init(&(pool->lock), NULL) != 0) ||//初始化线程池中锁 和 条件变量
           (pthread_cond_init(&(pool->notify), NULL) != 0) ||
           (pool->threads == NULL) ||
           (pool->queue == NULL)) {
            break; 
        }
    
         /*-----启动线程池中的线程-----*/
        for(i = 0; i < thread_count; i++) 
        {
            /* 启动线程池中每个线程，同时绑定线程运行的函数为threadpool_thread，该函数的参数是pool
               即每启动一个线程，就让其去执行threadpool_thread，从请求队列中，取出任务，并去执行，如果没有任务，则阻塞线程 */
            if(pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void*)pool) != 0) {
                threadpool_destroy(pool, 0);//如果没有创建成功则销毁
                return NULL;
            }
            pool->thread_count++; //如果创建成功则线程数目++
            pool->started++;//如果创建成功则start++
        }
          return pool;
    } while(false);
    
    if (pool != NULL) {  //如果线程池创建失败则释放

        threadpool_free(pool);
    }
    return NULL;
}

//向请求队列中添加任务，这个请求队列是所有线程共享的，所以要保证线程同步
int threadpool_add(threadpool_t *pool, void (*function)(void *), void *argument, int flags) {
    
    int err = 0;
    int next;

    if(pool == NULL || function == NULL){
        return THREADPOOL_INVALID;
    }
    if(pthread_mutex_lock(&(pool->lock)) != 0){//加锁
        return THREADPOOL_LOCK_FAILURE;
    }
    next = (pool->tail + 1) % pool->queue_size;//更新请求队列中最后一个任务索引
    do 
    {
        //如果请求队列中任务数量已满
        if(pool->count == pool->queue_size) {
            err = THREADPOOL_QUEUE_FULL;
            break;
        }
        
        //检查线程池状态
        if(pool->shutdown) {
            err = THREADPOOL_SHUTDOWN;
            break;
        }
        
        //向请求队列中添加任务
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next;
        pool->count += 1;
        
        //发送唤醒信号，如果任务数量足够多，线程没有休眠，即使消费者收到了信号也不会做任何处理
        if(pthread_cond_signal(&(pool->notify)) != 0) {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }
    } while(false);

    if(pthread_mutex_unlock(&pool->lock) != 0) {//解锁
        err = THREADPOOL_LOCK_FAILURE;
    }

    return err;
}

// 按照正常流程，摧毁线程池
int threadpool_destroy(threadpool_t *pool, int flags){
    printf("Thread pool destroy !\n");
    int i, err = 0;

    if(pool == NULL){
        return THREADPOOL_INVALID;
    }

    if(pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREADPOOL_LOCK_FAILURE;
    }

    do {

        // 线程池已经关闭了，重复关闭
        if(pool->shutdown) {
            err = THREADPOOL_SHUTDOWN;
            break;
        }

        pool->shutdown = (flags & THREADPOOL_GRACEFUL) ? graceful_shutdown : immediate_shutdown;

        /* -----唤醒所有工作线程----- */
        if((pthread_cond_broadcast(&(pool->notify)) != 0) || (pthread_mutex_unlock(&(pool->lock)) != 0)) {
            err = THREADPOOL_LOCK_FAILURE;
            break;
        }

        /* -----回收所有工作线程资源----- */
        for(i = 0; i < pool->thread_count; ++i)
        {
            if(pthread_join(pool->threads[i], NULL) != 0){
                err = THREADPOOL_THREAD_FAILURE;
            }
        }
    } while(false);

    /* -----只有所有事情都执行完毕后，才能释放线程池 */
    if(!err) {
        threadpool_free(pool);
    }
    return err;
}

//直接释放线程池
int threadpool_free(threadpool_t *pool)
{
    if(pool == NULL || pool->started > 0){
        return -1;
    }

    if(pool->threads) {//如果线程还未释放
        free(pool->threads);
        free(pool->queue);
 
        /*  因为在初始化互斥锁和条件变量之后才分配了pool->threads，所以互斥锁和条件变量一定已经初始化了
            在这里以防万一，先加锁，释放锁，再释放互斥量 */
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
    }
    free(pool);    
    return 0;
}

// 线程绑定函数：从请求队列中，取出第一个任务，并去执行；如果没有任务，则阻塞
static void *threadpool_thread(void *threadpool)
{
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;
    for(;;){
        /* 由于请求队列，所有线程都可以访问，所以要进行线程同步
           根据条件变量和互斥锁线程调度抢占    */ 
        pthread_mutex_lock(&(pool->lock));//加互斥锁避免多个线程占用条件变量

        /* 如果线程池中的任务队列暂时任务数量为0，使用pthread_cond_wait根据条件变量阻塞等待
           当任务数量大于0时，wait解除阻塞，再次while()，由于pool->count!=0而退出while循环，进入任务处理环节
           
           疑问：如果一个线程加锁以后，被wait阻塞了，为什么生产者那边还能继续加锁添加任务呢？
                因为wait函数内部在调用阻塞的时候，会对互斥锁进行解锁，
                这样子生产者那边才可以继续加锁生产数据，解锁发送唤醒信号，
                当wait函数收到唤醒信号后解除阻塞，继续向下执行，会重新加锁   */
        while((pool->count == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

     /* ----- 该线程获取执行权以后,开始执行任务处理环节----- */
        //如果此时线程池已经标志关闭了，则该线程退出
        if((pool->shutdown == immediate_shutdown) ||  ((pool->shutdown == graceful_shutdown) && (pool->count == 0))) {
            break;
        }
  
        //程序能到这里，表示线程池还没有标志关闭，则从任务队列头取任务来处理 
        task.function = pool->queue[pool->head].function;// 指定任务的处理函数
        task.argument = pool->queue[pool->head].argument;// 指定任务的处理函数的参数
        
        //取出任务后，要将任务从队列中删除
        pool->head = (pool->head + 1) % pool->queue_size;// 调整队列头
        pool->count -= 1;//减少队列数目
        pthread_mutex_unlock(&(pool->lock));   //释放互斥锁 

        (*(task.function))(task.argument);// 执行取出的任务
        
    }

    --pool->started;
    pthread_mutex_unlock(&(pool->lock));
    pthread_exit(NULL);
    return(NULL);
}