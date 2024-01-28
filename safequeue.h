#pragma once

#include <queue>
#include <mutex>
#include <chrono>
#include <atomic>
#include <condition_variable>
template<typename T>
class SafeQueue
{
private:
    /* data */
    std::atomic<bool> m_abort;
    std::mutex m_qmutex;
    std::condition_variable m_cv;
    std::queue<T> m_q;
public:
    SafeQueue() : m_abort(false) {}
    ~SafeQueue() {}
    void Abort()
    {
        m_abort.store(true);
        m_cv.notify_all();
    }

    void Resume()
    {
        m_abort.store(false);
        m_cv.notify_all();
    }
    bool Push(T val)
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        if (m_abort)
        {
            return false;
        }
        m_q.push(val);
        m_cv.notify_all();
        return true;
    }

    bool PushMove(T&& val)
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        if (m_abort)
        {
            return false;
        }
        m_q.push(std::move(val));
        m_cv.notify_all();
        return true;
    }

    bool PopRelease(T& t)
    {
        std::unique_lock<std::mutex> lk(m_qmutex);
        if (m_q.empty())
        {
            return false;
        }
        t = m_q.front();
        m_q.pop();
        return true;
    }

    bool PopMove(T& t, int timeout = 0)
    {
        std::unique_lock<std::mutex> lk(m_qmutex);
        if (m_q.empty())
        {
            if (timeout <= 0)
                return false;

            m_cv.wait_for(lk, std::chrono::milliseconds(timeout),
                [this] { return !m_q.empty() || m_abort; });
        }
        if (m_abort || m_q.empty())
        {
            return false;
        }
        t = std::move(m_q.front());
        m_q.pop();
        return true;
    }

    bool Pop(T& val, int timeout = 0)
    {
        std::unique_lock<std::mutex> lk(m_qmutex);
        if (m_q.empty())
        {
            if (timeout <= 0)
                return false;

            m_cv.wait_for(lk, std::chrono::milliseconds(timeout),
                [this] { return !m_q.empty() || m_abort; });
        }
        if (m_abort || m_q.empty())
        {
            return false;
        }
        val = m_q.front();
        m_q.pop();
        return true;
    }

    bool Front(T& val)
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        if (m_abort)
        {
            return false;
        }
        if (m_q.empty())
        {
            return false;
        }
        val = m_q.front();
        return true;
    }

    int Size()
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        return m_q.size();
    }
    void Clear()
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        while(m_q.size()) m_q.pop();
    }
    int Floors(int floors)
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        int discard = 0;
        while (m_q.size() > floors)
        {
            m_q.pop();
            ++discard;
        }
        return discard;
    }
    int Discard(int discard)
    {
        std::lock_guard<std::mutex> lk(m_qmutex);
        int i = 0;
        while (i < discard)
        {
            m_q.pop();
            i ++;
        }
        return i;
    }
};

// 线程安全的双端队列
template<typename T>
class SafeDoubleQueue
{
private:
    std::deque<T> m_q;
    std::mutex m_qMtx;
public:
    SafeDoubleQueue() {}
    ~SafeDoubleQueue() {}
    void pushBack(const T& t)
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        m_q.push_back(t);
    }
    void pushFront(const T& t)
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        m_q.push_front(t);

    }
    T popBack()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        T t = m_q.back();
        m_q.pop_back();
        return t;
    }
    T popFornt()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        T t = m_q.front();
        m_q.pop_front();
        return t;
    }
    T& front()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        return m_q.front();
    }
    T& back()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        return m_q.back();
    }
    bool empty()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        return m_q.empty();
    }
    int size()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        return m_q.size();
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(m_qMtx);
        return m_q.clear();
    }
};
