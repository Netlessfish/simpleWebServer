#include "http_conn.h"


#include "http_conn.h"

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/sylvia/simpleServerWeb/resources";//资源路径

// 所有的客户数
int http_conn::m_user_count = 0;
// 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
int http_conn::m_epollfd = -1;

// 设置文件描述符非阻塞
int setnonblocking( int fd ) {
    int old_flag = fcntl( fd, F_GETFL );
    int new_flag = old_flag | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_flag );
    return old_flag;
}

//添加文件描述符到epoll中
void addfd( int epollfd, int fd, bool one_shot ) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;//边沿触发
    if(one_shot) 
    {
        // 防止多个线程同时处理同一个通信socket
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    setnonblocking(fd);  
}

//从epollfd中删除文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;//初始化以后就可以在整个http_conn类中对这个文件描述符进行操作
    m_address = addr;
    
    // 端口复用
    int reuse = 1;
    setsockopt( m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );

    //将新连接的客户端的fd添加到epoll监听对象中
    addfd( m_epollfd, sockfd, true );
    m_user_count++;//总用户数+1
    init();//把两个init()分开写，方便重复利用init()，不需要反复传入套接字地址
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始状态为解析请求首行
    m_checked_idx = 0;   
    m_start_line = 0;      //当前正在解析的行的索引 
    m_read_idx = 0;
    m_write_idx = 0;

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;              
    m_version = 0;

    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接
    m_content_length = 0;
    m_host = 0;
  
   
    bzero(m_read_buf, READ_BUFFER_SIZE);//将读缓冲区清空，全部置为0
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn() {
    if(m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);//从epoll监听对象中删除
        m_sockfd = -1;
        m_user_count--; // 关闭一个连接，将客户总数量-1
    }
}

// 循环读取客户数据，直到无数据可读或者对方关闭连接
bool http_conn::read() {
    //缓冲区已经满了
    if( m_read_idx >= READ_BUFFER_SIZE ) {
        return false;
    }

    //已经读取到的字节
    int bytes_read = 0;
    while(true) {
        // 从m_read_buf + m_read_idx索引出开始保存数据，大小是READ_BUFFER_SIZE - m_read_idx
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0 );
        if (bytes_read == -1) {
            if( errno == EAGAIN || errno == EWOULDBLOCK ) {//非阻塞的读，没有数据也会返回-1，需要单独判断
                // 没有数据
                break;
            }
            return false;   
        } else if (bytes_read == 0) {   // 对方关闭连接
            return false;
        }
        //程序到这里表示已经读到数据，更新索引
        m_read_idx += bytes_read;
    }
    printf("读取到了数据：%s\n",m_read_buf);
    return true;
}



/*主状态机流程：解析请求
先获取一行数据parse_line()，
根据检查的状态m_check_state，做不同的处理（解析请求行，头还是请求体）
每个不同的处理，它的状态也会发生改变
*/
http_conn::HTTP_CODE http_conn::process_read() {

    //定义一些初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;//初始状态：最终解析的结果
    char* text = 0;//获取的一行数据

    //使用while循环一行一行的解析数据
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
                || ((line_status = parse_line()) == LINE_OK)) {
                    //parse_line()为获取一行数据

        // 程序到这里表示，解析到了一行完整的数据，或者解析到了请求体，也是完整的数据

        //获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;//将下一次检查位置的起始位置 更新为 当前的检查位置
        printf( "got 1 http line: %s\n", text );

        //主状态机
        switch ( m_check_state ) {
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                }
                break;
            }
            //解析完头部，或者解析完头部后面的请求体，都有可能是解析完全部请求，从而状态转移为GET_REQUEST，去执行do_request()
            case CHECK_STATE_HEADER: {
                ret = parse_headers( text );
                if ( ret == BAD_REQUEST ) {
                    return BAD_REQUEST;
                } else if ( ret == GET_REQUEST ) {//请求完成
                    return do_request();//解析具体的内容
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                ret = parse_content( text );
                if ( ret == GET_REQUEST ) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 解析一行，判断依据\r \n
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for ( ; m_checked_idx < m_read_idx; ++m_checked_idx ) {
        temp = m_read_buf[ m_checked_idx ];
        if ( temp == '\r' ) {//http请求报文中，每一行是数据XXXX\r\n
            if ( ( m_checked_idx + 1 ) == m_read_idx ) {// 仅读到XXXX\r
                return LINE_OPEN;//数据不完整，因为\r的下一位应该是\n，而不是下一次读取位置
            } else if ( m_read_buf[ m_checked_idx + 1 ] == '\n' ) {//如果当前是\r，下一个是\n，说明当前和下一个分别是\r\n，换行
                m_read_buf[ m_checked_idx++ ] = '\0';//将当前检查位置'\r'置为'\0'，并移动到下一位
                m_read_buf[ m_checked_idx++ ] = '\0';//将当前检查位置'\n'置为'\0'，并移动到下一行
                return LINE_OK;
            }
            return LINE_BAD;
        } else if( temp == '\n' )  {
            /*http请求报文 
                 xxxx\r\n
                 XXXXXXXX\r\n
            */
            if( ( m_checked_idx > 1) && ( m_read_buf[ m_checked_idx - 1 ] == '\r' ) ) {
                //这里判断m_checked_idx > 1，是为了m_checked_idx - 1不超出索引范围
                //如果当前是\n，前一个是\r，表示也是一个换行
                m_read_buf[ m_checked_idx-1 ] = '\0';//将前一个位置\r置为\0
                m_read_buf[ m_checked_idx++ ] = '\0';//将当前位置\n置为\0，并移动到下一行
                return LINE_OK;
            }
            return LINE_BAD;//如果没有读到完整的一行数据，返回LINE_BAD，然后下次继续读取到数据了再来解析一行数据
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行，获得请求方法，目标URL,以及HTTP版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // http报文的第一行数据 GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    // /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;// 192.168.110.129:10000/index.html
        
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );// /index.html
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 主状态机 检查状态变成检查头
    return NO_REQUEST;//解析完请求首行，但还没有解析完所有，所以返回NO_REQUEST
}

// 解析HTTP请求的一个头部信息
/*
Host:192.168.193.128:10000\0\0
Connection:keep-alive
Content-Length:1000
*/
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {   
    // 遇到空行，表示头部字段解析完毕
    if( text[0] == '\0' ) {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if ( m_content_length != 0 ) {//说明有请求体
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 如果没有请求体，说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if ( strncasecmp( text, "Connection:", 11 ) == 0 ) {//忽略大小写比较
        // 处理Connection 头部字段  Connection: keep-alive
        text += 11;
        text += strspn( text, " \t" );
        if ( strcasecmp( text, "keep-alive" ) == 0 ) {
            m_linger = true;
        }
    } else if ( strncasecmp( text, "Content-Length:", 15 ) == 0 ) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn( text, " \t" );
        m_content_length = atol(text);
    } else if ( strncasecmp( text, "Host:", 5 ) == 0 ) {
        // 处理Host头部字段
        text += 5;
        text += strspn( text, " \t" );
        m_host = text;
    } else {
        printf( "oop! unknow header %s\n", text );
    }
    return NO_REQUEST;
}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content( char* text ) {
    if ( m_read_idx >= ( m_content_length + m_checked_idx ) )
    {
        text[ m_content_length ] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}



// 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
// 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn::HTTP_CODE http_conn::do_request()
{
    // "/home/sylvia/simpleServerWeb/resources"
    strcpy( m_real_file, doc_root );//把资源路径"/home/sylvia/simpleServerWeb/resources"拷贝到m_real_file中
    int len = strlen( doc_root );
    strncpy( m_real_file + len, m_url, FILENAME_LEN - len - 1 );//拼接"/home/sylvia/simpleServerWeb/resources/index.html"
    
    // 获取m_real_file文件的相关的状态信息，-1失败，0成功
    if ( stat( m_real_file, &m_file_stat ) < 0 ) {
        return NO_RESOURCE;
    }

    // 判断访问权限
    if ( ! ( m_file_stat.st_mode & S_IROTH ) ) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if ( S_ISDIR( m_file_stat.st_mode ) ) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );//释放资源
        m_file_address = 0;
    }
}

// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写是指：有多块不连续的数据，一起写出去
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modfd( m_epollfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();//写完以后，要释放掉
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {//第一个参数format是格式，后面...是可变参数
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {//写满了
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {//缓冲区写不下
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

//添加响应状态行
bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();//加空行
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;//封装第一块内存，头信息之类的
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;//封装的第二块内存，客户请求的资源
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;//一共有两块内存
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if ( read_ret == NO_REQUEST ) {//请求不完整，需要继续读取客户数据
        modfd( m_epollfd, m_sockfd, EPOLLIN );//需要重新修改EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
        return;
    }
    
    // 生成响应
    bool write_ret = process_write( read_ret );
    if ( !write_ret ) {
        close_conn();
    }
    //因为使用了oneshot，所以每次操作完，不管结果如何，都得重新设置监听事件
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}
