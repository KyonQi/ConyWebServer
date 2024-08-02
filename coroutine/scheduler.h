#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include <string>
#include <vector>
#include <list>
#include <functional>

#include "fiber.h"
#include "thread.h"
#include "../log/log.h"
#include "../lock/locker.h"

class Scheduler {
public:
    typedef std::shared_ptr<Scheduler> ptr;

    /**
     * @brief 构造函数，创建调度器
     * @param[in] threads 线程数
     * @param[in] use_caller 将创造调度器的线程用于调度线程
     * @param[in] name 调度器名字
     */
    Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "Scheduler");
    virtual ~Scheduler();

    const std::string& getName() const { return m_name; }
    
    static Scheduler* GetThis(); //获取当前线程的调度器指针 多个属于同调度器的指针应当指向一个调度器
    
    static Fiber* GetMainFiber(); //获取当前线程的主协程 对于非use_caller而言，应当是调度协程

    /**
     * @brief 添加调度任务
     * @tparam FiberOrCb 调度任务类型，因为可能是fiber或func，所以用模板
     * @param[] fc 协程对象或函数指针
     * @param[] thread 指定运行人任务的线程号，-1为任意线程
     */
    template <class FiberOrCb>
    void schedule(FiberOrCb fc, int thread = -1) {
        bool need_tickle = false;
        m_mutex.lock();
        need_tickle = scheduleNoLock(fc, thread);
        m_mutex.unlock();
        
        if (need_tickle) {
            tickle();
        }

    }

    void start();
    void stop(); //注意use_caller的调度协程应当在stop再唤醒？

protected:
    virtual void tickle(); //通知协程调度器有任务

    virtual void idle(); //无任务调度时执行idle协程
    
    virtual bool stopping(); //返回调度器是否可以停止

    void run(); //协程调度函数，schedule()相当于添加任务进队列，run()相当于从队列取出任务

    void setThis(); //设置当前运行的协程调度器

    bool hasIdleThreads() { return m_idleThreadCount > 0; }

private:
    /**
     * @brief 真正的添加调度任务到队列中，锁在调用前添加，自行解锁
     */
    template <class FiberOrCb>
    bool scheduleNoLock(FiberOrCb fc, int thread) {
        bool need_tickle = m_tasks.empty(); //如果队列为空，应当唤醒添加任务到队列中
        ScheduleTask task(fc, thread); //封装成 ScheduleTask类型
        if (task.fiber || task.cb) {
            m_tasks.push_back(task);
        }
        return need_tickle;
    }

private:
    /**
     * @brief 封装单独的fiber协程或者函数
     */
    struct ScheduleTask {
        Fiber::ptr fiber;
        std::function<void()> cb; //待封装的fiber/cb
        int thread;

        ScheduleTask(Fiber::ptr f, int thr) {
            fiber = f;
            thread = thr;
        }
        ScheduleTask(Fiber::ptr *f, int thr) {
            //这里因为是swap，使用指针f作为参数，保证原始ptr的控制权能够被转移，引用计数保证不变
            fiber.swap(*f);
            thread = thr;
        }
        ScheduleTask(std::function<void()> f, int thr) {
            cb = f;
            thread = thr;
        }
        ScheduleTask() {
            thread = -1;
        }

        void reset() {
            fiber = nullptr;
            cb = nullptr;
            thread = -1;
        }
        
    };
    

private:
    std::string m_name; //调度器名字
    locker m_mutex; //互斥锁
    std::vector<Thread::ptr> m_threads; //线程池，这里后续应当和TinyWebserver合并
    std::list<ScheduleTask> m_tasks; //任务队列
    std::vector<int> m_threadIds; //线程池的线程ID数组
    size_t m_threadCount = 0; //工作线程数量
    std::atomic<size_t> m_activeThreadCount = {0}; //活跃线程数
    std::atomic<size_t> m_idleThreadCount = {0}; //不活跃线程数

    bool m_useCaller; //Scheduler是否使用本体所在线程参与调度
    Fiber::ptr m_rootFiber; //userCaller为1时，本体线程的调度协程，只有本体线程会用到
    int m_rootThread = 0;

    bool m_stopping = false; //是否停止调度器
};

#endif