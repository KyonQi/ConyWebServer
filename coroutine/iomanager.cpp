#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include "iomanager.h"
#include "../log/log.h"

static int m_close_log = 0;

//获取事件的上下文，根据读、写分别返回对应的事件
IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(IOManager::Event event) {
    switch (event) {
        case IOManager::READ:
            return read;
        case IOManager::WRITE:
            return write;
        default:
            LOG_ERROR("%s", "IOManager::getEventContext event False");
            LOG_ASSERT(false);
    }
    throw std::invalid_argument("getContext invalid event");
}

//重置事件的上下文
void IOManager::FdContext::resetEventContext(EventContext &ctx) {
    ctx.scheduler = nullptr;
    ctx.fiber.reset(); // ctx.fiber是Fiber智能指针形式
    ctx.cb = nullptr;
}

//触发Fd对应事件
void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    LOG_ASSERT(events & event); //events存储了Fd关注的事件，event是想要触发的事件，即触发事件必须此前注册过
    events = (Event) (events & ~event); // 将触发的事件从关注事件中清除，即注册IO是一次性的
    EventContext &ctx = getEventContext(event); //根据event将对应事件上下文取出
    //触发事件实际上就是加入到调度器中，调度器下次在schedule::run调度执行回调函数or协程
    if (ctx.cb) {
        ctx.scheduler->schedule(ctx.cb);
    } else {
        ctx.scheduler->schedule(ctx.fiber);
    }
    resetEventContext(ctx);
    return ;
}

// IOManager在初始化的时候就直接启动调度
IOManager::IOManager(int &epfd, size_t threads, bool use_caller, const std::string &name)
            : Scheduler(threads, use_caller, name) {
    m_epfd = epfd;
    //m_epfd = epoll_create(5000);
    LOG_ASSERT(m_epfd > 0);

    int rt = pipe(m_tickleFds); //创建tickle的通道，m_tickleFds[0]为读，1为写
    LOG_ASSERT(!rt);

    epoll_event event; //注册tickle的读事件
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET; //注册可读事件，边缘触发
    event.data.fd = m_tickleFds[0];
    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK); // 将tickle的fd设置成非阻塞模式
    LOG_ASSERT(!rt);
    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event);
    LOG_ASSERT(!rt);

    contextResize(32);
    
    start(); // IOManager在初始化的时候就直接启动调度器
}

IOManager::~IOManager() {
    stop(); //先关闭调度器
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (m_fdContexts[i]) {
            delete m_fdContexts[i];
        }
    }
}

int IOManager::setnonblocking(int fd) {
    //返回给定文件描述符 fd 的文件状态标志。这些标志通常用于描述文件的打开方式（例如只读、只写、读写等）以及其他特性（例如非阻塞模式）
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// m_fdContexts扩容，注意这个vector直接存储了对应下标Fd的上下文(FdContext指针)
void IOManager::contextResize(size_t size) {
    m_fdContexts.resize(size); // 这是vector变量 直接resize
    
    for (size_t i = 0; i < m_fdContexts.size(); ++i) {
        if (!m_fdContexts[i]) {
            m_fdContexts[i] = new FdContext;
            m_fdContexts[i]->fd = i;
        }
    }
}

//添加fd关心的事件(读/写)
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) {
    // 首先找到fd对应的上下文FdContext
    FdContext *fd_ctx = nullptr;
    m_iomutex.rdlock();
    if ((int) m_fdContexts.size() > fd) {
        // 如果容量大于fd，那直接从中取出上下文即可，注意vector里存的是上下文指针
        fd_ctx = m_fdContexts[fd];
        m_iomutex.unlock();
    } else {
        m_iomutex.unlock();
        
        // vector里没有则需要扩容，换成写锁
        m_iomutex.wrlock();
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
        m_iomutex.unlock();
    }

    //找到Fd对应的上下文后，向其中添加新的event
    fd_ctx->mutex.lock();
    //同一个fd，不能重复添加相同的事件
    if (fd_ctx->events & event) {
        LOG_ERROR("IOManager::addEvent same event added, event=%d, fd_ctx.events=%d", (EPOLL_EVENTS)event, (EPOLL_EVENTS)fd_ctx->events);
        //fd_ctx->mutex.unlock();
        LOG_ASSERT(!(fd_ctx->events & event));
    }

    Event new_events = (Event) (fd_ctx->events | event);
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD; //如果fd先前注册了某种事件那就MOD，否则ADD
    epoll_event epevent; // 准备添加的新事件
    epevent.events = EPOLLET | new_events; 
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent); //将fd的事件注册到epoll中
    if (rt) {
        LOG_ERROR("IOManager::addEvent epoll_ctl add event False, fd=%d", fd);
        fd_ctx->mutex.unlock();
        return -1;
    }
    setnonblocking(fd);

    ++m_pedingEventCount; //待执行IO事件加1
    fd_ctx->events = new_events; // 加入新注册进epoll的event
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    LOG_ASSERT(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb); //新添加的event_ctx内各成员都应是nullptr
    event_ctx.scheduler = Scheduler::GetThis();
    if (cb) {
        event_ctx.cb.swap(cb);
    } else {
        //如果回调函数为空，则把当前协程当成回调执行体，那么初始化主协程
        event_ctx.fiber = Fiber::GetThis(); 
        LOG_ASSERT(event_ctx.fiber->getState() == Fiber::RUNNING); //主协程状态在初始化的时候就是RUNNING
    }
    fd_ctx->mutex.unlock();
    return 0;
}

//删除fd关心的事件
bool IOManager::delEvent(int fd, Event event) {
    m_iomutex.rdlock();
    //首先找到fd对应的FdContext
    if ((int) m_fdContexts.size() <= fd) {
        m_iomutex.unlock();
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    m_iomutex.unlock();

    fd_ctx->mutex.lock();
    //如果fd的FdContext不包含要删除的事件，直接返回false
    if (!(fd_ctx->events & event)) {
        fd_ctx->mutex.unlock();
        return false;
    }

    //清除指定的事件，如果清除结果为0，则直接从epoll_wait中将fd删除
    Event new_events = (Event) (fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent; //新的事件
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        LOG_ERROR("IOManager::delEvent false, fd=%d, event=%d", fd, event);
        fd_ctx->mutex.unlock();
        return false;
    }

    --m_pedingEventCount; //待执行的IO事件减1
    fd_ctx->events = new_events;
    FdContext::EventContext &event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx); //取出被删除的事件上下文，重置
    fd_ctx->mutex.unlock();
    return true;
}

//取消fd关心的事件，取消时会触发一次事件
bool IOManager::cancelEvent(int fd, Event event) {
    //找到fd对应的FdContext
    m_iomutex.rdlock();
    if ((int) m_fdContexts.size() <= fd) {
        m_iomutex.unlock();
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    m_iomutex.unlock();

    fd_ctx->mutex.lock();
    if (!(fd_ctx->events & event)) {
        LOG_ERROR("IOManager::cancelEvent false, fd=%d", fd);
        fd_ctx->mutex.unlock();
        return false;
    }
    //取消事件，注意和删除不同，取消会触发一次事件
    Event new_events = (Event) (fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = EPOLLET | new_events;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) {
        LOG_ERROR("IOManager::cancelEvent false, fd=%d, event=%d", fd, event);
        fd_ctx->mutex.unlock();
        return false;
    }
    fd_ctx->triggerEvent(event); //在triggerEvent里，就有fd_ctx->events的更新，所以这里不更新
    --m_pedingEventCount;
    fd_ctx->mutex.unlock();
    return true;
}

//取消Fd的所有事件，取消时会触发所有事件
bool IOManager::cancelAll(int fd) {
    m_iomutex.rdlock();
    if ((int) m_fdContexts.size() <= fd) {
        m_iomutex.unlock();
        return false;
    }
    FdContext *fd_ctx = m_fdContexts[fd];
    m_iomutex.unlock();

    fd_ctx->mutex.lock();
    //如果fd本身没有注册任何事件，直接返回false
    if (!fd_ctx->events) {
        fd_ctx->mutex.unlock();
        return false;
    }

    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events = 0;
    epevent.data.ptr = fd_ctx;
    int rt = epoll_ctl(m_epfd, op, fd, &epevent); //从epoll中取消所有注册的事件
    if (rt) {
        LOG_ERROR("IOManager::cancelAll false, fd=%d", fd);
        fd_ctx->mutex.unlock();
        return false;
    }

    //取消时将所有注册的事件都要触发，触发的同时会更新fd_ctx的events成员
    if (fd_ctx->events & READ) {
        fd_ctx->triggerEvent(READ);
        --m_pedingEventCount;
    }
    if (fd_ctx->events & WRITE) {
        fd_ctx->triggerEvent(WRITE);
        --m_pedingEventCount;
    }

    LOG_ASSERT(fd_ctx->events == 0);
    fd_ctx->mutex.unlock();
    return true;
}

IOManager* IOManager::GetThis() {
    return dynamic_cast<IOManager *> (Scheduler::GetThis()); //使用dynamic_cast实现类的下行转换(因为有虚函数存在)
}

/**
 * @brief 通知IO协程调度
 * @details 通过tickle，idle的协程会退出，进入Scheduler::run进行调度，如果没有进入到idle的协程就不需要tickle
 */
void IOManager::tickle() {
    LOG_DEBUG("IOManager::tickle %s", "TICKLE");
    if (!hasIdleThreads()) {
        //如果不存在idle线程，那么直接退出即可
        return ;
    }
    int rt = write(m_tickleFds[1], "T", 1); //往tickle的通道里写一个字符，让epoll返回唤醒idle
    LOG_ASSERT(rt == 1);
}

/**
 * @brief IOManager的暂停需要等待所有IO调度事件执行完毕才行
 */
bool IOManager::stopping() {
    return m_pedingEventCount == 0 && Scheduler::stopping();
}

/**
 * @brief IOManager的idle协程
 * @details 调度器无任务时会阻塞在idle协程中，即epoll_wait中，直到一：出现新的调度任务加入调度器触发tickle，
 * 二：fd关注的IO事件被触发；这两个都会触发epoll_wait返回
 */
void IOManager::idle() {
    LOG_DEBUG("IOManager::idle %s", "START");

    const uint64_t MAX_EVENTS = 256; //一次epoll_wait最多返回256个就绪事件
    epoll_event *events = new epoll_event[MAX_EVENTS](); //创建返回的事件数组
    std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr) {
        delete[] ptr;
    }); 
    //因为shared_ptr管理的是一个动态分配的数组，需要自定义删除器实现delete[]，而非delete，利用智能指针防止内存泄漏;
    //lambada 自定义删除器格式 [](参数列表) -> 返回类型 { 函数体 }

    while (true) {
        //如果IOManager被执行了stop，那么应当从idle中退出
        if (stopping()) {
            LOG_DEBUG("IOManager::idle name=%s, idle stopping exit", getName().c_str());
            break;
        }
        //正常idle阻塞在epoll_wait中
        static const int MAX_TIMEOUT = 5000; //阻塞时间 -1就会永久阻塞在这 TIMEOUT后rt返回0
        int rt = epoll_wait(m_epfd, events, MAX_EVENTS, MAX_TIMEOUT); //idle阻塞
        if (rt < 0) {
            if (errno == EINTR) {
                //LOG_DEBUG("%s", "IOManager::idle CONTINUE!");
                continue;
            }
            LOG_ERROR("IOManager::idle, epoll_wait fd=%d, rt=%d, erro=%s", m_epfd, rt, strerror(errno));
            break;
        }
        //遍历所有发生的事件，根据epoll_event.data.ptr找到对应的FdContext，从而触发事件的回调
        for (int i = 0; i < rt; ++i) {
            epoll_event &event = events[i];
            if (event.data.fd == m_tickleFds[0]) {
                //如果是tickle的读端，把管道内容读完即可
                uint8_t dummy[256];
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0) {
                    ;
                }
                continue;
            }

            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            fd_ctx->mutex.lock();
            //触发EPOLLERR：写了读端已经关闭的pipe
            //触发EPOLLHUP：socket对端关闭
            //这两种情况发生时，应当同时触发fd的读和写(如果注册了)，否则可能出现注册的事件永远执行不到
            if (event.events & (EPOLLERR | EPOLLHUP)) {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events; //触发所有注册的读或写
            }
            //event.events返回的是本次触发的事件，而fd_ctx.events是fd关注的所有事件
            int real_events = NONE;
            if (event.events & EPOLLIN) {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) {
                real_events |= WRITE;
            }

            if ((fd_ctx->events & real_events) == NONE) {
                //这种情况，已经有其他线程处理了fd关注的eventl1，直接跳过该fd
                continue;
            }

            int left_events = (fd_ctx->events & ~real_events); //剩下没触发的事件，应当重新修改进epoll
            int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events = EPOLLET | left_events;
            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) {
                LOG_ERROR("IOManager::idle left_events false, epfd=%d, fd=%d, error=%s", m_epfd, fd_ctx->fd, strerror(errno));
                continue;
            }

            if (real_events & READ) {
                fd_ctx->triggerEvent(READ); //将fd对应读事件的回调加入到调度器中
                --m_pedingEventCount;
            }
            if (real_events & WRITE) {
                fd_ctx->triggerEvent(WRITE);
                --m_pedingEventCount;
            }
            fd_ctx->mutex.unlock();    
        } 
        //处理完所有epoll返回的事件，idle协程应当yield返回，从而使得Scheduler::run能够重新进入调度流程
        Fiber::ptr cur = Fiber::GetThis();
        auto raw_ptr = cur.get();
        cur.reset();
        raw_ptr->yield();
    } //while结束
}









