# simpleServerWeb

#### 介绍
> version0.0 学习版本，是基于游双的《linux高性能服务器编程》，将定时器合并入原代码中

version1.0 基于Reactor模式的simpleWebServer

test_pressure 压力测试工具

#### 使用说明
1. 克隆到本地
2. 编译

```
cd simpleServerWeb/version1.0
g++ *.cpp -o simpleServerWeb -pthread
./simpleServerWeb
```
3. 打开地址栏输入

```
127.0.0.1：8888
```

#### 软件架构

##### 软件架构说明

待补充。。。


#### 特性

1. 使用Epoll边沿触发的IO多路复用技术，非阻塞IO，使用Reactor模式
2. 使用多线程充分利用多核CPU，并使用线程池避免线程频繁创建销毁的开销
3. 利用状态机思想解析Http报文，支持GET/POST请求，支持长/短连接
4. 使用基于小根堆的定时器关闭超时请求 解决超时;连接系统资源占用问题

#### 压力测试

```
./webbench -t 30 -c 1000 -2 --get http://127.0.0.1:8888/
```
测试了并发1000个get请求，压测30s,长连接

结果如下：

待补充。。。
