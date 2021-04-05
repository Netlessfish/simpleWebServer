#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"

/*
对线程池进行封装
线程池的作用是寻找一个线程处理任务
*/

//线程池类，定义成模板类是为了代码的复用，模板参数T是任务类，使得代码也可以去处理其他任务
template<typename T>
class threadpool{
public:
    threadpool(int thread_number = 8, int max_request = 10000);
    /*
    threadpool构造函数中不仅传入thread_number和max_request，
    还在threadpool成员变量中定义m_thread_number,m_max_requests,
    是为了其他成员函数也可以使用这两个值，如果不定义成员变量的话，
    那么其他成员函数如果想要使用这两个值，只能在每个函数参数中不停的传入
    */
    ~threadpool();
    bool append(T* request);//添加任务

private:
    static void* worker(void * arg);
    void run();//将线程池启动

private:
    //线程的数量
    int m_thread_number;

    //线程池数组，大小为m_thread_number
    pthread_t *m_threads;

    //请求队列中最多允许的，等待处理的请求数量
    int m_max_requests;

    //请求队列
    std::list<T*> m_workqueue;

    //互斥锁
    locker m_queuelocker;//请求队列是所有线程共享的，所以需要互斥锁

    //信号量用来判断是否有任务需要处理
    sem m_queuestat;

    //是否结束线程
    bool m_stop;

};

//成员函数具体实现
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_request)://：冒号后面可以对成员进程初始化
    m_thread_number(thread_number), m_max_requests(max_request), m_stop(false), m_threads(NULL){
    if((thread_number <= 0) || (max_request <= 0)){//先判断传入参数thread_number,max_request有没有违法
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_number];//通过new创建的，不要忘记delete掉
    if(!m_threads){
        throw std::exception();
    }

    //创建thread_number个线程，并将他们设置为线程脱离。因为被分离的线程在终止的时候，会自动释放资源返回给系统。
    for(int i=0; i<thread_number; ++i){
        printf("create the %dth thread\n",i);
        
        if( pthread_create(m_threads + i, NULL, worker, this) != 0){
            /*
            worker是子线程需要处理的逻辑代码，CPP中worker必须是一个静态的函数
            但是静态函数中不能访问非静态成员，所以worker没法使用threadpool中的成员变量，例如m_threads，m_thread_number等等
            所以这里将pthread_create（）函数中第四个参数设置为this，this将作为参数传入worker中
            */
            delete [] m_threads;
            throw std::exception();
        }

        if( pthread_detach(m_threads[i]) ){
            delete m_threads;
            throw std::exception();
        }
            
    }
}

template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;
    m_stop = true;//m_stop默认为false
    //根据这个值判断是否要停止线程，否则线程将要一直循环的运行，
}

template<typename T>
bool threadpool<T>::append(T * request){
    //往队列添加任务，一定要保证线程同步，因为这个是共享数据
    m_queuelocker.lock();//对任务队列上锁
    if(m_workqueue.size() > m_max_requests){
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);//添加队列
    m_queuelocker.unlock();//对任务队列解锁
    m_queuestat.post();//往队列中添加一个任务后，信号量value+1
    return true;
}

template<typename T>
//静态函数中不能访问非静态成员
void* threadpool<T>::worker(void * arg){
    threadpool * pool = (threadpool *)arg;//将传入的参数arg转化为threadpool*（即pthread_create（）函数的第四个参数this）
    pool->run();//执行线程池
    return pool;
}

template<typename T>
void threadpool<T>::run(){//线程池需要一直运行，直到m_stop==false
    //线程池run()的作用：需要从队列中取一个任务并执行
    while(!m_stop){
        m_queuestat.wait();//如果信号量！=0,就不阻塞；如果信号量==0，阻塞
        m_queuelocker.lock();//上锁
        if(m_workqueue.empty()){//如果任务队列是空的，将解锁，并且循环运行，直到有任务
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();//获取队列第一个任务
        m_workqueue.pop_front();//取出任务后，要将该任务从队列中删除
        m_queuelocker.unlock();//解锁

        if(!request){
            continue;
        }

        request->process();//取出的任务 执行
    }
}

#endif