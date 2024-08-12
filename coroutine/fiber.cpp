#include <atomic>

#include "fiber.h"
#include "scheduler.h"
#include "../log/log.h"

static int m_close_log = 1; //LOG类使用变量后续移除

//全局静态变量，用于生成协程ID
static std::atomic<uint64_t> s_fiber_id{0};
//全局静态变量，用于统计当前协程数
static std::atomic<uint64_t> s_fiber_count{0};

//线程局部变量，当前线程正在运行的协程
static thread_local Fiber *t_fiber = nullptr;
//线程局部变量，当前线程的主协程————只接收主-子 两个协程非对称的切换，智能指针形式
static thread_local Fiber::ptr t_thread_fiber = nullptr;

//协程栈默认大小 128k
static uint32_t fiber_stack_size = 128 * 1024;

/**
 * @brief 封装malloc和free的协程栈内存分配器
 */
class MallocateStackAllocator {
public:
    static void *Alloc(size_t size) {return malloc(size);}
    static void Dealloc(void *vp, size_t size) {return free(vp);}
};

using StackAllocator = MallocateStackAllocator;

uint64_t Fiber::GetFiberId() {
    //注意取得是当前正在运行的协程ID
    if (t_fiber) {
        return t_fiber->getId();
    }
    return 0;
}

//将线程当前运行的协程设置成参数f
void Fiber::SetThis(Fiber *f) {
    t_fiber = f;
}

Fiber::Fiber() {
    SetThis(this); //将创建的新主协程设置为当前线程正在运行的
    m_state = RUNNING;

    if (getcontext(&m_ctx)) {
        //LOG_ERROR("%llu Fiber Main getcontext wrong", s_fiber_id);
    }

    s_fiber_count++;
    m_id = s_fiber_id++; //这个主协程的id

    LOG_DEBUG("Fiber::Fiber main id = %llu", m_id);
}

/**
 * @brief 获取当前协程，同时充当初始化当前线程主协程的作用
 * 
 * @return Fiber::ptr 返回当前线程的协程
 */
Fiber::ptr Fiber::GetThis() {
    if (t_fiber) {
        //如果当前线程有正在运行的协程
        return t_fiber->shared_from_this();
    }
    Fiber::ptr main_fiber(new Fiber); //类内调用private的构造，初始化主协程
    //LOG_DEBUG("Judge if it's main fiber : %d", t_fiber == main_fiber.get());
    t_thread_fiber = main_fiber; //t_thread_fiber是线程的主协程，智能指针形式
    //注意，这里主协程的创建不分配栈空间
    return t_fiber->shared_from_this();
}

/**
 * @brief 带参数的构造，用于创建子协程，需要分配调用栈空间
 * @param cb 协程的回调入口函数
 * @param stacksize 分配的调用栈空间大小
 */
Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) 
    : m_id(s_fiber_id++), m_cb(cb), m_runInScheduler(run_in_scheduler) {
    s_fiber_count++;
    m_stacksize = stacksize ? stacksize : fiber_stack_size;
    m_stack = StackAllocator::Alloc(m_stacksize); //分配协程的栈空间
    //初始化并拿到当前协程上下文
    if (getcontext(&m_ctx)) {
        //LOG_ERROR("%llu Fiber getcontext wrong", s_fiber_id);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack; //栈顶指针
    m_ctx.uc_stack.ss_size = m_stacksize; //调用栈大小
    
    makecontext(&m_ctx, &Fiber::MainFunc, 0); //修改get初始化的context，设置MainFunc(封装后的m_cb)为context的入口函数
    //之后再调用swapcontext或者setcontext，绑定的MainFunc就会被激活
    LOG_DEBUG("Fiber::Fiber main id = %llu", m_id);
}

/**
 * @brief 因为主协程和子协程创建方式不同，主协程不分配栈空间和cb
 * 因此，主、子协程需要分开判断析构
 * 
 */
Fiber::~Fiber() {
    LOG_DEBUG("Fiber::~Fiber() id = %llu", m_id);
    s_fiber_count--;
    if (m_stack) {
        //如果分配了栈空间，说明不是线程的主协程，要解除分配的空间
        if (m_state != TERM) {
            LOG_ERROR("Fiber::~Fiber error not TERM id = %llu", m_id);
        }
        StackAllocator::Dealloc(m_stack, m_stacksize);
        LOG_DEBUG("Fiber::~Fiber deallocate stack id = %llu", m_id);
    } else {
        //没有分配栈空间，说明是线程的主协程，将主协程的指针置空即可
        //注意，一定要在主协程是RUNNING状态自己置空
        Fiber *cur = t_fiber; //线程运行的当前协程
        if (cur == this) {
            //如果当前协程是线程的主协程
            SetThis(nullptr);
        }
    }
}


/**
 * @brief 只有TERM状态的协程才能重置，重置复用栈空间，就不需要重新分配
 * 
 * @param cb 新协程的回调入口函数
 */
void Fiber::reset(std::function<void()> cb) {
    if (!m_stack) LOG_ERROR("Fiber::reset no stack, id = %llu", m_id);
    if (m_state != TERM) LOG_ERROR("Fiber::reset fiber not TERM id = %llu", m_id);
    m_cb = cb; //拿到重置的回调，重新构建context即可
    if (getcontext(&m_ctx)) {
        //LOG_ERROR("%llu Fiber getcontext wrong", s_fiber_id);
    }

    m_ctx.uc_link = nullptr;
    m_ctx.uc_stack.ss_sp = m_stack; //复用上个协程的栈空间，因此只需要改cb
    m_ctx.uc_stack.ss_size = m_stacksize;

    makecontext(&m_ctx, &Fiber::MainFunc, 0);
    m_state = READY; //改变状态
    LOG_DEBUG("Fiber::reset %llu is now reset", m_id);
}

/**
 * @brief 切换到当前协程继续运行，注意区分是否由调度器参与，若是则应与调度器主协程(调度协程)切换
 * 若否，则应与线程主协程切换
 */
void Fiber::resume() {
    if (m_state == TERM || m_state == RUNNING) LOG_ERROR("Fiber::resume %llu is TERM or RUNNING, can't resume", m_id);
    LOG_ASSERT(m_state != TERM && m_state != RUNNING);
    //LOG_INFO("Fiber %llu is now resume", m_id); // for testing

    SetThis(this);
    m_state = RUNNING;
    //swap会直接跳转执行，并不会返回，相当于调用函数
    if (m_runInScheduler) {
        if (swapcontext(&(Scheduler::GetMainFiber()->m_ctx), &m_ctx)) {
            //如果是Scheduler支配的协程，应当与Scheduler的调度协程做调换
            //适用于线程池内的协程
            LOG_ERROR("Fiber::resume %s", "Scheduler Resume Fault");
        }
    } else {
        if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
            //如果非Scheduler支配，应当与线程主协程（主线程）做调换
            //适用于use_caller线程
            LOG_ERROR("Fiber::resume %s", "Main Thread Resume Fault");
        }
    }
    // if (swapcontext(&(t_thread_fiber->m_ctx), &m_ctx)) {
    //     LOG_ERROR("Fiber:: resume %llu swap false", m_id);
    // } // Version 1.0
}

void Fiber::yield() {
    if (m_state != TERM && m_state != RUNNING) LOG_ERROR("Fiber::yield %llu not TERM or RUNNING, can't yield, curr state is %d", m_id, m_state);
    LOG_ASSERT(m_state == RUNNING || m_state == TERM);
    //LOG_INFO("Fiber %llu is now yield", m_id);

    SetThis(t_thread_fiber.get()); //当前协程切换回主协程
    if (m_state != TERM) {
        m_state = READY; //如果被切换掉的协程没有结束，则转回READY等待被调度
    }

    // 如果协程参与调度器，则应与调度器主协程swap，否则应与线程主协程
    if (m_runInScheduler) {
        if (swapcontext(&m_ctx, &(Scheduler::GetMainFiber()->m_ctx))) {
            LOG_ERROR("Fiber::yield %s", "Scheduler Yield Fault");
        }
    } else {
        if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
            LOG_ERROR("Fiber::yield %s", "Main Thread Yield Fault");
        }
    }
    // if (swapcontext(&m_ctx, &(t_thread_fiber->m_ctx))) {
    //     LOG_ERROR("Fiber::yield %llu false", m_id);
    // } // Ver. 1
}

void Fiber::MainFunc() {
    Fiber::ptr cur = GetThis(); //cur是智能指针形式
    if (!cur) {
        LOG_ERROR("Fiber::MainFunc cur is nullptr");
    }
    LOG_ASSERT(cur);

    cur->m_cb(); //调用当前协程的回调函数
    cur->m_cb = nullptr;
    cur->m_state = TERM; //处理完毕

    auto raw_ptr = cur.get();
    cur.reset(); //引用计数减1，cur不再管理
    raw_ptr->yield(); //让出协程使用权
}


















