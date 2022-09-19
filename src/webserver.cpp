//
// Created by nie on 22-9-7.
//

#include <arpa/inet.h>
#include "webserver.h"


WebServer::WebServer() {
    users = new http_conn[MAX_FD];

    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = new char[strlen(server_path) + strlen(root) + 1];
    strcpy(m_root, server_path);
    strcat(m_root, root);

    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}


void WebServer::log_write() {
    if (0 == m_close_log) {
        if (1 == m_log_write) {
            Log::get_instance()->init("./log/ServerLog", m_close_log, 2000, 800000, 800);
        } else {
            Log::get_instance()->init("./log/ServerLog", m_close_log, 2000, 800000, 0);
        }
    }
}

//epoll触发模式组合
void WebServer::trig_mode() {
    //LT + LT
    if (0 == m_triggermode) {
        m_listenTriggermode = 0;
        m_connTriggermode = 0;
    }
        //Lt + ET
    else if (1 == m_triggermode) {
        m_listenTriggermode = 0;
        m_connTriggermode = 1;
    }
        //ET + LT
    else if (2 == m_triggermode) {
        m_listenTriggermode = 1;
        m_connTriggermode = 0;
    }
        //ET + ET
    else if (3 == m_triggermode) {
        m_listenTriggermode = 1;
        m_connTriggermode = 1;
    }
}

void WebServer::eventListen() {
    //网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    //优雅关闭连接
    if (0 == m_opt_linger) {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else if (1 == m_opt_linger) {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *) &address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    //epoll创建内核事件表
//    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_listenTriggermode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, Utils::sig_handler, false);
    utils.addsig(SIGTERM, Utils::sig_handler, false);

    alarm(TIMESLOT);

    //工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::sql_pool() {
    m_connPool = sql_connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool() {
    m_pool = new threadpool<http_conn>(m_connPool, m_thread_num);
}


//创建一个定时器节点，将连接信息挂载
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    users[connfd].init(connfd, client_address, m_root, m_connTriggermode, m_close_log, m_user, m_passWord,
                       m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    auto *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(nullptr);
    //TIMESLOT:最小时间间隔单位为5s
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.timer_heap.add_timer(timer);
}

//若数据活跃，则将定时器节点往后延迟3个时间单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer) {
    time_t cur = time(nullptr);
    timer->expire = cur + 3 * TIMESLOT;
    utils.timer_heap.adjust_timer(timer);
    LOG_INFO("%s", "adjust timer once");
}

//删除定时器节点，关闭连接
void WebServer::deal_timer(util_timer *timer, int sockfd) {
    timer->cb_func(&users_timer[sockfd]);
    if (timer) {
        utils.timer_heap.del_timer(timer);
    }
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

//http 处理用户数据
bool WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    //LT
    if (0 == m_listenTriggermode) {
        int connfd = accept(m_listenfd, (struct sockaddr *) &client_address, &client_addrlength);
        if (connfd < 0) {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

        // ET
    else {
        //边缘触发需要一直accept直到为空
        while (true) {
            int connfd = accept(m_listenfd, (struct sockaddr *) &client_address, &client_addrlength);
            if (connfd < 0) {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

//处理定时器信号,set the timeout ture
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server) {
    int ret = 0;
    int sig;
    char signals[1024];
    //从管道读端读出信号值，成功返回字节数，失败返回-1
    //正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1) {
        // handle the error
        return false;
    } else if (ret == 0) {
        return false;
    } else {
        //处理信号值对应的逻辑
        for (int i = 0; i < ret; ++i) {

            //这里面明明是字符
            switch (signals[i]) {
                //这里是整型
                case SIGALRM: {
                    timeout = true;
                    break;
                }
                    //关闭服务器
                case SIGTERM: {
                    stop_server = true;
                    break;
                }
            }
        }
    }
    return true;
}

//处理客户连接上接收到的数据
void WebServer::dealwithread(int sockfd) {
    //创建定时器临时变量，将该连接对应的定时器取出来
    util_timer *timer = users_timer[sockfd].timer;
    if (timer) {
        //将定时器往后延迟3个单位
        adjust_timer(timer);
    }
    //若监测到读事件，将该事件放入请求队列
    m_pool->append(users + sockfd, 0);
    while (true) {
        //是否正在处理中
        if (1 == users[sockfd].improv) {
            //事件类型关闭连接
            if (1 == users[sockfd].timer_flag) {
                deal_timer(timer, sockfd);
                users[sockfd].timer_flag = 0;
            }
            users[sockfd].improv = 0;
            break;
        }
    }
}

//写操作
void WebServer::dealwithwrite(int sockfd) {
    util_timer *timer = users_timer[sockfd].timer;
    if (timer) {
        adjust_timer(timer);
    }
    m_pool->append(users + sockfd, 1);
    while (true) {
        if (1 == users[sockfd].improv) {
            if (1 == users[sockfd].timer_flag) {
                deal_timer(timer, sockfd);
                users[sockfd].timer_flag = 0;
            }
            users[sockfd].improv = 0;
            break;
        }
    }
}

//事件回环（即服务器主线程）
void WebServer::eventLoop() {
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server) {
        //等待所监控文件描述符上有事件的产生
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        //EINTR错误的产生：当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时，该系统调用可能返回一个EINTR错误。
        //例如：在socket服务器端，设置了信号捕获机制，有子进程，
        //当在父进程阻塞于慢系统调用时由父进程捕获到了一个有效信号时，内核会致使accept返回一个EINTR错误(被中断的系统调用)。
        //在epoll_wait时，因为设置了alarm定时触发警告，导致每次返回-1，errno为EINTR，对于这种错误返回
        //忽略这种错误，让epoll报错误号为4时，再次做一次epoll_wait
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        //对所有就绪事件进行处理
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;
            //处理新到的客户连接
            if (sockfd == m_listenfd) {
                bool flag = dealclinetdata();
                if (!flag)
                    continue;
            }
                //处理异常事件
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
                //处理定时器信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
                //接收到SIGALRM信号，timeout设置为True
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
                //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            }
                //处理客户连接上send的数据
            else if (events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }

        //处理定时器为非必须事件，收到信号并不是立马处理
        //完成读写事件后，再进行处理
        if (timeout) {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }
}

void WebServer::init(int port, std::string user, std::string passWord, std::string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log) {

    m_port = port;
    m_user = std::move(user);
    m_passWord = std::move(passWord);
    m_databaseName = std::move(databaseName);
    m_log_write = log_write;
    m_opt_linger = opt_linger;
    m_triggermode = trigmode;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_close_log = close_log;
}
