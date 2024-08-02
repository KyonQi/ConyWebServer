#ifndef __FIBER_H__
#define __FIBER_H__

#include <functional>
#include <memory>
#include <ucontext.h>
//#include "thread.h"

class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    typedef std::shared_ptr<Fiber> ptr;
    enum State {
        //定义协程3种状态
        READY,
        RUNNING,
        TERM
    };

private:
    /**
     * @brief 私有无参构造
     * @details 只用于线程创建第一个协程，即线程主函数对应的协程，
     * 该协程只能通过GetThis()方法调用，由enable_shared_from_this<>实现
     */
    Fiber(); 

public:
    /**
     * @brief 公有带参构造，用于构建子协程
     * @param cb 协程入口函数
     * @param stacksize 协程栈大小
     */
    Fiber(std::function<void()> cb, size_t stacksize = 0, bool run_in_scheduler = true);
    
    ~Fiber();

    void reset(std::function<void()> cb); //重置协程状态和入口函数，复用栈空间，不重复创建栈

    void resume(); //将当前协程恢复到执行状态

    void yield(); //当前协程让出执行权,当前协程状态转为READY，切换的转为RUNNING

    uint64_t getId() const {return m_id;} //获取当前协程id

    State getState() const {return m_state;} //获取当前协程状态

public:
    /**
     * @brief 返回当前线程正在执行的协程
     * @details 如果当前线程尚未创建协程，则创建第一个协程且作为主协程；
     * 因此，如果要创建协程应当首先执行GetThis
     * 
     * @return Fiber::ptr 
     */
    static Fiber::ptr GetThis();

    static void SetThis(Fiber *f); //设置当前正在运行的协程，即设置线程局部变量t_fiber

    static uint64_t TotalFibers(); //线程内总协程数目

    static void MainFunc(); //协程入口函数，对传入的cb进行二次封装

    static uint64_t GetFiberId(); //获取当前协程id

private:
    uint64_t m_id = 0;
    uint32_t m_stacksize = 0;
    State m_state = READY; //协程创建默认READY状态
    ucontext_t m_ctx; //该协程的上下文
    void *m_stack = nullptr; //协程的栈地址位置
    std::function<void()> m_cb; //协程的回调函数

    bool m_runInScheduler; //判断本协程是否受调度期支配 scheduler用于判断swap的对象
};



#endif