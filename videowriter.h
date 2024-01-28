#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <string>
#include <vector>
#include <thread>
#include <opencv2/opencv.hpp>
#include "safequeue.h"

#define MIN_SUBTITLE_WIDTH 100

class FFVideoEncodeThread
{
public:
    FFVideoEncodeThread() : bExit(true), bStopped(true), writer(NULL), runner(NULL), pipe_cmd(), sendnum(0)
    {
    }
    ~FFVideoEncodeThread()
    {
    }

    void RUN();

    // write data to file/fifo
    //   - type: 0 - raw, 1 - video, 2 - audio
    int Write(const unsigned char *data, int length, int type, int video_frame_no = 0);

    void STOP(bool force = false)
    {
        bStopped.store(true);
        videoBuffers.Abort();
        if(force)
        {
            videoBuffers.Clear();
        }
    }
    void START(FILE *fd)
    {
        writer = fd;                                                                                
        sendnum = 0;
        videoBuffers.Resume();
        bExit.store(false);
        bStopped.store(false);
        if (runner == NULL)
            runner = new std::thread(&FFVideoEncodeThread::RUN, this);
    }
    void START(const char *pipe, void *starter, const std::string &fifo)
    {
        writer = NULL;
        sendnum = 0;
        pipe_cmd = pipe;
        audio_starter = starter;
        audio_fifo = fifo;
        videoBuffers.Resume();
        bExit.store(false);
        bStopped.store(false);
        if (runner == NULL)
            runner = new std::thread(&FFVideoEncodeThread::RUN, this);
    }
    void EXIT(bool force = false)
    {
        bExit.store(true);
        if (force)
            videoBuffers.Clear();
        if (runner)
        {
            if (force)
            {
                pthread_kill((pthread_t)runner->native_handle(), SIGUSR1);
            }
            else
            {
                if(runner->joinable())
                {
                    runner->join();
                }
                delete runner;
                runner = NULL;
            }
        }
    }
public:
    std::atomic_bool bStopped, bExit;
    SafeQueue<std::vector<unsigned char>> videoBuffers;
    FILE *writer;
    std::thread *runner;
    std::string pipe_cmd;
    int64_t sendnum;
    void *audio_starter;
    std::string audio_fifo;
};


class FFAudioEncodeThread
{
public:
    FFAudioEncodeThread() : bExit(true), bStopped(true), writer(-1), runner(NULL)
    {
    }
    ~FFAudioEncodeThread()
    {
    }
    void RUN();

    int Write(const unsigned char *data, int length)
    {
        std::vector<unsigned char> buf(length);
        memcpy(buf.data(), data, length);
        audioBuffers.PushMove(std::move(buf));
        return 0;
    }
    void STOP(bool force = false)
    {
        bStopped.store(true);
        audioBuffers.Abort();
        if(force)
            audioBuffers.Clear();
    }
    void START(const std::string &fifo)
    {
        fifo_name = fifo;
        audioBuffers.Resume();
        bExit.store(false);
        bStopped.store(false);
        if (runner == NULL)
            runner = new std::thread(&FFAudioEncodeThread::RUN, this);
    }
    void EXIT(bool force = false)
    {
        bExit.store(true);
        if (force && runner)
        {
            audioBuffers.Clear();
            pthread_kill((pthread_t)runner->native_handle(), SIGUSR1);
            // if (runner)
            // {
            //     if(runner->joinable())
            //     {
            //         runner->join();
            //     }
            //     delete runner;
            //     runner = NULL;
            // }
        }
    }
public:
    std::atomic_bool bStopped, bExit;
    SafeQueue<std::vector<unsigned char>> audioBuffers;
    int writer;
    std::thread *runner;
    std::string fifo_name;
};


struct FFVideoRawData
{
    int encode_type;
    int frame_number;
    std::vector<unsigned char> frame;
};

struct AVFrame;
struct SwsContext;
struct AVFilterContext;

class FFSubtitleEncodeThread
{
public:
    FFSubtitleEncodeThread() : bExit(false), bStopped(false), runner(NULL), lastFrameNo(-1)
    {
    }
    ~FFSubtitleEncodeThread()
    {
    }

    void RUN();

    int Read(std::vector<unsigned char> &data, int &frame_no, int out_framenum, int out_fps)
    {
        if (skip_subtitle)
            return -1;

        if (lastFrameNo > 0)
        {
            double out_timebase = ((double)out_framenum) / out_fps;
            double sub_timebase = ((double)lastFrameNo + 0.5) / video_fps;
            if (sub_timebase >= out_timebase)
            {
                frame_no = out_framenum;
                return 0;
            }
        }

        FFVideoRawData rawdata;
        if (finishedBuffers.PopMove(rawdata, 100))
        {
            data = rawdata.frame;
            frame_no = rawdata.frame_number;
            lastFrameNo = rawdata.frame_number;
        }
        return 0;
    }
    void STOP(bool force = false)
    {
        bStopped.store(true);
        if(force)
            finishedBuffers.Clear(); 
    }

    // open subtitle decoder and start rendering thread
    // supported types: srt, ssa, ass, lrc, etc.
    // return 0 on success, -1 on failure to load subtitle file
    int START(const std::string &file, const std::string &style, int fps,
              int video_width, int video_height,
              int sub_x, int sub_y, int sub_w, int sub_h);

    void EXIT(bool force = false)
    {
        finishedBuffers.Clear(); 
        bExit.store(true);
        // if(runner->joinable())
        // {
        //     runner->join();
        // }
        // delete runner;
        // runner = NULL;
    }

private:
    bool init_subtitle_filter(AVFilterContext *&buffersrc, AVFilterContext *&buffersink,
                              const std::string &args, const std::string &filterDesc);

public:
    int align, sub_x, sub_y, sub_w, sub_h;

private:
    std::atomic_bool bStopped, bExit;
    SafeQueue<FFVideoRawData> finishedBuffers;
    std::thread *runner;
    std::string file_name;

    // video parameters
    int video_fps, video_width, video_height, video_pixfmt;

    // video context
    AVFrame *frame;
    AVFrame *displayFrame;
    int buffersize;
    std::vector<unsigned char> displayBuffer;
    std::vector<unsigned char> frameBuffer;

    // subtitle context
    bool skip_subtitle;
    AVFrame *filterFrame;
    SwsContext *swsContext;
    AVFilterContext *buffersrcContext;
    AVFilterContext *buffersinkContext;
    int lastFrameNo;
};

extern FFVideoEncodeThread ffVideoEncodeThread;
extern FFAudioEncodeThread ffAudioEncodeThread;
extern FFSubtitleEncodeThread ffSubtitleEncodeThread;

