#ifndef __THREAD_H__
#define __THREAD_H__

#include <thread>
#include <functional>
#include <memory>
#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <atomic>
#include <list>

#include "../lock/locker.h"


class Thread;

static thread_local Thread *t_thread = nullptr;
static thread_local std::string t_thread_name = "UnKnow";

class Noncopyable {
public:
    /**
     * @brief 默认构造函数
     */
    Noncopyable() = default;

    /**
     * @brief 默认析构函数
     */
    ~Noncopyable() = default;

    /**
     * @brief 拷贝构造函数(禁用)
     */
    Noncopyable(const Noncopyable&) = delete;

    /**
     * @brief 赋值函数(禁用)
     */
    Noncopyable& operator=(const Noncopyable&) = delete;
};


class Thread : Noncopyable {
public:
    typedef std::shared_ptr<Thread> ptr;
    Thread(std::function<void()> cb, const std::string &name) 
    : m_cb(cb), m_name(name) {
    //cb是线程执行函数
    if (name.empty()) {
        m_name = "UnKnow";
    }
    int rt = pthread_create(&m_thread, nullptr, &run, this);
    if (rt) {
        throw std::logic_error("pthread_create error");
    }
    m_sem.wait();
    }

    ~Thread() {
        if (m_thread) {
            pthread_detach(m_thread);
        }
    }
    
    pid_t getId() const { return m_id; }
    
    const std::string& getNane() const { return m_name; }

    void join() {
        if (m_thread) {
            int rt = pthread_join(m_thread, nullptr);
            if (rt) {
                throw std::logic_error("pthread_join error");
            }
            m_thread = 0;
        }
    }

    static Thread* GetThis() {
        return t_thread;
    }

    static const std::string& GetName() {
        return t_thread_name;
    }

    static void SetName(const std::string& name) {
        if (name.empty()) return ;
        if (t_thread) {
            t_thread->m_name = name;
        }
        t_thread_name = name;
    }

    static pid_t GetThreadId() { return syscall(SYS_gettid); }

private:
    static void* run(void *arg) {
        Thread *thread = (Thread *)arg;
        t_thread = thread;
        t_thread_name = thread->m_name;
        thread->m_id = syscall(SYS_gettid);
        pthread_setname_np(pthread_self(), thread->m_name.substr(0, 15).c_str());

        std::function<void()> cb;
        cb.swap(thread->m_cb);
        thread->m_sem.post();
        cb();
        return 0;
    }

private:
    pid_t m_id = 1;
    pthread_t m_thread = 0;
    std::function<void()> m_cb;
    std::string m_name;
    sem m_sem;
};




#endif