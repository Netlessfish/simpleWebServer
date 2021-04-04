#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <exception>
#include <semaphore.h>

//线程同步机制封装类(对所有线程同步所需要的的锁，条件变量，信号量进行封装)

//互斥锁类
class locker{
public:
    locker(){//锁构造函数
        if(pthread_mutex_init(&m_mutex, NULL) != 0){
            throw std::exception();//抛出异常
        }
    }
    ~locker(){//锁析构函数
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock(){//上锁
        return pthread_mutex_lock(&m_mutex);
    }
    bool unlock(){//解锁
        return pthread_mutex_unlock(&m_mutex);
    }
    pthread_mutex_t *get(){//获取一个互斥量
        return &m_mutex;
    }
private:
    pthread_mutex_t m_mutex;
};

//条件变量
class cond{
public:
     cond(){//条件变量构造函数
         if(pthread_cond_init(&m_cond, NULL) != 0){
             throw std::exception();//抛出异常
         }
     }
     ~cond(){
         pthread_cond_destroy(&m_cond);
     }
     bool wait(pthread_mutex_t *mutex){
         return pthread_cond_wait(&m_cond, mutex) == 0;
     }
     bool timedwait(pthread_mutex_t *mutex, struct timespec t){
         return pthread_cond_timedwait(&m_cond, mutex,&t) == 0;
     }
     bool signal(){//唤醒一个或者多个线程
         return pthread_cond_signal(&m_cond) == 0;
     }
     bool broadcast(){//将所有线程都唤醒
         return pthread_cond_signal(&m_cond) == 0;
     }
private:
    pthread_cond_t m_cond;
};

//信号量
class sem{
public:
    //信号量有两种构造方式
    sem(){
        if(sem_init(&m_sem, 0, 0) != 0){
            throw std::exception();//抛出异常
        }
    }
    sem(int num){//num为信号量中的value
        if(sem_init(&m_sem, 0, num) != 0){
            throw std::exception();//抛出异常
        }
    }
    ~sem(){
        sem_destroy(&m_sem);
    }
    bool wait(){//等待信号量，对信号量加锁，调用一次对信号量的value-1，如果value为0，就阻塞
        return sem_wait(&m_sem) == 0;
    }
    bool post(){//增加信号量，每调用一次value+1
        return sem_post(&m_sem) == 0;
    }
private:
    sem_t m_sem;
};

#endif