#include <sys/syscall.h>

#include "scheduler.h"

static int m_close_log = 0;

// 当前线程的调度器，统一调度器下所有线程共享同一实例
static thread_local Scheduler *t_scheduler = nullptr;
// 当前线程的调度协程，每个线程独有一份
static thread_local Fiber *t_scheduler_fiber = nullptr;

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name) {
    LOG_ASSERT(threads > 0);
    m_useCaller = use_caller;
    m_name = name;

    if (m_useCaller) {
        --threads; //因为创建调度器的线程也参与调度了
        Fiber::GetThis(); //初始化主协程 很重要！
        LOG_ASSERT(GetThis() == nullptr); //这里这个GetThis是Scheduler的静态函数，返回当前线程的调度器，确保是空指针
        t_scheduler = this;

        //useCaller的调度协程创建，因为不受调度器支配(非t_scheduler_fiber)，放在这里
        //注意最后一个参数run_in_scheduler是false
        //因为useCaller调度协程运行完后，应当返回线程主协程(上面的GetThis)，否则程序会跑飞
        m_rootFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false));
        Thread::SetName(m_name);
        t_scheduler_fiber = m_rootFiber.get();
        m_rootThread = syscall(SYS_gettid); //创建调度器线程的序号
        m_threadIds.push_back(m_rootThread);
    } else {
        m_rootThread = -1; //不使用创建调度器的线程参与调度时，设置该线程序号为-1
    }
    m_threadCount = threads; //使用线程的个数
}

Scheduler* Scheduler::GetThis() {
    return t_scheduler; //返回当前线程的调度器实例
}

Fiber* Scheduler::GetMainFiber() {
    return t_scheduler_fiber; //返回当前线程的调度协程 对于子线程，是调度协程——子协程 这样切换
}

void Scheduler::setThis() {
    t_scheduler = this; //将线程调度器设置成当前调度器
}

Scheduler::~Scheduler() {
    LOG_DEBUG("Scheduler::~Scheduler %s is deleting", m_name.c_str());
    LOG_ASSERT(m_stopping); //必须m_stopping为1，才能析构
    if (GetThis() == this) {
        t_scheduler = nullptr;
    }
}

void Scheduler::start() {
    LOG_DEBUG("Scheduler::start %s", m_name.c_str());
    m_mutex.lock(); //必须上锁，因为多线程操作m_threadIds以及判断m_stopping
    if (m_stopping) {
        LOG_ERROR("Scheduler::start %s ERROR", m_name);
        return ;
    }
    
    LOG_ASSERT(m_threads.empty()); //调度器启动的时候，保证已有线程应当是空的
    m_threads.resize(m_threadCount); //resize成需要的线程个数
    for (size_t i = 0; i < m_threadCount; ++i) {
        m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), 
                                        m_name + "_" + std::to_string(i)));
        m_threadIds.push_back(m_threads[i]->getId());
    }
    m_mutex.unlock();
}

bool Scheduler::stopping() {
    m_mutex.lock();
    bool stopping = m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
    m_mutex.unlock();
    return stopping;
}

void Scheduler::tickle() {
    LOG_DEBUG("Scheduler::tickle %s", "TICKLE NOW");
}

/**
 * @brief //如果调度器不能停止，说明队列还有task没有完成 或者 调度器没有stop
 *        //yield根据是否受调度器调度，进行swapcontext，去到调度协程中
 */
void Scheduler::idle() {
    LOG_DEBUG("Sscheduler::idle %s", "IDLE NOW");
    while (!stopping()) {
        Fiber::GetThis()->yield(); //返回当前线程正在执行的协程
    }
}

void Scheduler::stop() {
    LOG_DEBUG("Scheduler::stop %s", "STOP NOW");
    if (stopping()) {
        return ;
    }

    m_stopping = true;
    //如果启用了use_caller，那么只能由use_caller线程发起stop方法
    LOG_DEBUG("Scheduler::stop TEST Scheduler::GetThis == this ? %d", GetThis() == this);
    if (m_useCaller) {
        LOG_ASSERT(GetThis() == this); //通过GetThis获得当前线程的调度器
    } else {
        LOG_ASSERT(GetThis() != this); //因为如果没有设置主线程参与的话，主线程GetThis应该是nullptr
    }

    for (size_t i = 0; i < m_threadCount; ++i) {
        tickle(); //相当于唤醒每个线程
    }
    if (m_rootFiber) {
        tickle(); //如果使用了use_caller，那么应当专门唤醒这个线程
    }

    //在use_caller情况下，调度器协程结束时，应当返回caller协程
    if (m_rootFiber) {
        m_rootFiber->resume();
        LOG_DEBUG("Scheduler::stop %s", "m_rootFiber End");
    }

    std::vector<Thread::ptr> thrs;
    m_mutex.lock();
    thrs.swap(m_threads); //目的是在stop的时候，提前释放掉Scheduler内m_threads的空间
    m_mutex.unlock();

    for (auto &i : thrs) {
        i->join(); //等待子线程全部运行完毕
    }
}

/**
 * @brief Scheduler线程池内部绑定的都是run函数
 * 这个是协程调度函数
 */
void Scheduler::run() {
    LOG_DEBUG("Scheduler::run %s", "RUN BEGIN");
    setThis(); //将线程的调度器设置成当前调度器
    if (Thread::GetThreadId() != m_rootThread) {
        //当前线程并非创建调度器的主线程时
        t_scheduler_fiber = Fiber::GetThis().get(); //初始化调度协程
    }

    Fiber::ptr idle_fiber(new Fiber(std::bind(&Scheduler::idle, this))); //idle协程
    Fiber::ptr cb_fiber;

    ScheduleTask task;
    while (true) {
        task.reset();
        bool tickle_me = false; //是否tickle其他线程进行任务调度
        
        //下面上锁准备从list中取任务
        m_mutex.lock();
        auto it = m_tasks.begin(); //按顺序遍历任务丢给线程执行
        while (it != m_tasks.end()) {
            if (it->thread != -1 && it->thread != Thread::GetThreadId()) {
                //如果任务指定了线程运行，且不是当前线程，那么需要标记通知其他线程前往调度
                ++it; //当前线程直接跳过此任务
                tickle_me = true;
                continue;
            }

            //找到未制定线程，或者指定了当前线程的任务
            LOG_ASSERT(it->fiber || it->cb);
            if (it->fiber) {
                //协程任务开始时，状态必须是READY
                LOG_ASSERT(it->fiber->getState() == Fiber::READY);
            }
            task = *it; //拿到迭代器的内容
            m_tasks.erase(it++); //从list中清除掉这个task
            ++m_activeThreadCount;
            break;
        }
        m_mutex.unlock();

        if (tickle_me) {
            tickle(); //通知其他线程
        }

        if (task.fiber) {
            //如果任务是封装好的协程，那么应当resume执行
            //当协程返回时，要么执行完毕TERM，要么半路yield回到READY
            task.fiber->resume();
            --m_activeThreadCount; //执行完毕返回，减少活跃的线程数
            task.reset();
        } else if (task.cb) {
            //如果是未封装的函数，在这里将其封装成协程参与调度即可
            if (cb_fiber) {
                cb_fiber->reset(task.cb); //复用栈空间
            } else {
                cb_fiber.reset(new Fiber(task.cb)); //需要new分配栈空间
            }
            task.reset();
            cb_fiber->resume();
            --m_activeThreadCount;
            cb_fiber.reset(); //智能指针reset掉
        } else {
            //说明没有任务了，进入了这个分支，将当前线程进入到idle协程中
            if (idle_fiber->getState() == Fiber::TERM) {
                //若调度器没有任务，那么idle协程应当会不停resume-yield，不会结束
                //如果idle协程结束，那么一定是调度器停止了
                //通过这个地方，使得整个循环能有地方退出，很重要！！！
                LOG_DEBUG("Scheduler::run %s", "Idle Fiber TERM");
                break;
            }
            ++m_idleThreadCount;
            idle_fiber->resume();
            --m_idleThreadCount;
        }
    }
    LOG_DEBUG("Scheduler::run %s", "RUN EXIT");
}









