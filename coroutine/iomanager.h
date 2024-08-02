#ifndef __IOMANAGER_H__
#define __IOMANAGER_H__

#include "scheduler.h"


class IOManager : public Scheduler {
public: 
    typedef std::shared_ptr<IOManager> ptr;
    enum Event {
        NONE = 0x0,
        // EPOLLIN 可读
        READ = 0x1,
        // EPOLLOUT 可写
        WRITE = 0x4,
    };

private:
    /**
     * @brief socket fd上下文类，保存了每个fd对应的（读写）上下文
     * @details 包含了fd的值，fd事件，以及fd读写上下文
     */
    struct FdContext {

        // 事件的上下文，包含事件的调度器，回调函数，以及协程
        struct EventContext {
            Scheduler *scheduler = nullptr; //执行事件回调的调度器
            Fiber::ptr fiber; // 事件回调协程
            std::function<void ()> cb; // 事件回调函数
        };
        
        //获取事件上下文
        EventContext& getEventContext(Event event);
        //重置事件上下文
        void resetEventContext(EventContext &ctx);
        //触发事件
        void triggerEvent(Event event);

        EventContext read; //读事件上下文
        EventContext write; //写事件上下文
        int fd = 0; //事件的fd
        Event events = NONE;  //该fd关心哪些事件
        locker mutex; //事件的mutex锁
    };

public:
    /**
     * @brief 构造函数，参数原理与继承的Scheduler是一脉相承的
     */
    IOManager(int &epfd, size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();

    /**
     * @brief 添加事件，fd发生event事件时执行回调cb
     */
    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);

    /**
     * @brief 删除事件，将fd对应的event从epoll中删除
     */
    bool delEvent(int fd, Event event);

    /**
     * @brief 取消事件，将fd对应的event从epoll中取消，如果曾被注册过回调，则触发一次回调
     */
    bool cancelEvent(int fd, Event event);

    /**
     * @brief 取消所有注册事件
     */
    bool cancelAll(int fd);

    /**
     * @brief 返回当前的IOManager
     */
    static IOManager *GetThis();

    /**
     * @brief 将fd设置为非阻塞模式
     */
    int setnonblocking(int fd);

protected:
    /**
     * @brief 通知调度器有任务调度
     * @details 通过写pipiFd让idle协程从epoll_wait退出，随后scheduler::run重新调度任务
     */
    void tickle() override;

    /**
     * @brief 判断IOManager是否能够停止
     * @details 条件是 Scheduler::stopping() && m_pendingEventCount == 0，即没有协程调度，也没有IO调度
     */
    bool stopping() override;

    /**
     * @brief idle协程
     * @details IOManager中，idle阻塞在epoll_wait中，需要tickle或者注册的事件发生才会退出idle，重新进入调度
     */
    void idle() override;

    /**
     * @brief 重置fd上下文的容器大小，所有的fd上下文都存储在一个vector中
     */
    void contextResize(size_t size);

private:
    int m_epfd = 0; //epoll的fd
    int m_tickleFds[2]; //pipefd，fd[0]读,fd[1]写
    std::atomic<size_t> m_pedingEventCount = {0}; //等待执行的IO事件数量
    iolocker m_iomutex; //IOManager的锁
    std::vector<FdContext *> m_fdContexts; //socket事件上下文的容器
}; 

#endif