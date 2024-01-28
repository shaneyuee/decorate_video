#include <thread>
#include <vector>
#include <iostream>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>    
#include <sys/time.h>
#include "event.h"
#include "pb.h"
#include "safequeue.h"
#include "3rd/log/LOGHelp.h"

#undef	__MODULE__
#define __MODULE__ "Event"

using namespace std;

class PushEventThread
{
public:
    PushEventThread() : bExit(false), bStopped(false), writer(-1), runner(NULL)
    {
    }
    void RUN()
    {
        while (true)
        {
            if (bStopped)
            {
                if (bExit) // exit only when queue is empty
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (writer < 0)
            {
                writer = open(fifo_name.c_str(), O_WRONLY);
                if (writer < 0)
                {
                    char s[1024];
                    strerror_r(errno, s, sizeof(s));
                    LOG_ERROR("Open fifo %s failed, ret=%d:%s", fifo_name.c_str(), errno, s);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                LOG_INFO("Open envent notification fifo successfully.");
            }
            vector<unsigned char> buf;
            if (!eventBuffers.PopMove(buf)) // empty
            {
                if (bExit) // exit only when queue is empty
                {
                    close(writer);
                    writer = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            int ret = write(writer, buf.data(), buf.size());
            if(ret < 0)
            {
                char s[1024];
                strerror_r(errno, s, sizeof(s));
                LOG_ERROR("Error writing to event fifo, err=%d:%s", errno, s);
            }
        }
    }
    int Write(const unsigned char *data, int length)
    {
        vector<unsigned char> buf(length);
        memcpy(buf.data(), data, length);
        eventBuffers.PushMove(std::move(buf));
        return 0;
    }
    void STOP(bool force = false)
    {
        bStopped.store(true);
        if(force)
            eventBuffers.Clear();
    }
    void START(const std::string &fifo)
    {
        fifo_name = fifo;
        bExit.store(false);
        bStopped.store(false);
        if (runner == NULL)
            runner = new std::thread(&PushEventThread::RUN, this);
    }
    void EXIT(bool force = false)
    {
        bExit.store(true);
        if (!force)
        {
            if(runner->joinable())
            {
                runner->join();
            }
            delete runner;
            runner = NULL;
        }
    }
public:
    std::atomic_bool bStopped, bExit;
    SafeQueue<vector<unsigned char>> eventBuffers;
    int writer;
    std::thread *runner;
    std::string fifo_name;
} pushEventThread;


int init_fifo_event(const char *pipe_name)
{
    pushEventThread.START(pipe_name);
    return 0;
}

int finish_fifo_event()
{
    pushEventThread.EXIT(true);
    return 0;
}

int64_t getCurrentTime()
{
    struct timeval tv;    
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int send_event(EventCode code, const std::string &msg)
{
    LOG_INFO("Sending event code=%d, msg=%s", code, msg.c_str());
    if (pushEventThread.fifo_name.empty() || pushEventThread.runner == NULL)
    {
        LOG_INFO("Event fifo not initialzied.");
        return -1;
    }

    pb_buff_t pb;
    uint8_t buf[msg.length()+32];
    // packet format    4Byte     4Byte     4Byte     0or4Byte   VarLen
    // little endian   Version + MsgType + BodyLen + ExtHeader + Body
    ((int *)buf)[0] = 1; // version
    ((int *)buf)[1] = EC_EVENT_DECORATE_SDK; // decorate video SDK events
    ((int *)buf)[2] = 0; // header length
    pb_init_buff(&pb, buf+sizeof(MsgHead), msg.length()+32-sizeof(MsgHead));

    // Body part is a pb message
    // message DecorateEvent
    // {
    //     int64 timestamp = 1;  // 时间戳 毫秒级
    //     int32 eventCode = 2;  // 合成事件码
    //     string eventMsg = 3;  // 合成事件描述
    // }
    pb_add_varint(&pb, 1, getCurrentTime());
    pb_add_varint(&pb, 2, code);
    pb_add_string(&pb, 3, msg.data(), msg.size());
    ((int *)buf)[2] = pb_get_encoded_length(&pb); // header length

    return pushEventThread.Write(buf, ((int *)buf)[2]+sizeof(MsgHead));
}
