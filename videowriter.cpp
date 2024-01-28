#include <string>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "event.h"
#include "videowriter.h"
#include "AutoTime.h"
#include "material.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
}
#include "3rd/log/LOGHelp.h"

#undef	__MODULE__
#define __MODULE__ "VideoWriter"


FFVideoEncodeThread ffVideoEncodeThread;
FFAudioEncodeThread ffAudioEncodeThread;
FFSubtitleEncodeThread ffSubtitleEncodeThread;

using namespace std;
extern int enable_debug;

void FFVideoEncodeThread::RUN()
{
    vector<unsigned char> buf;
    while (true)
    {
        if (bStopped || writer==NULL)
        {
            if (bExit) // exit only when queue is empty
                break;
            if (!bStopped && writer==NULL && pipe_cmd.length())
            {
                LOG_INFO("popen %s", pipe_cmd.c_str());
                writer = popen(pipe_cmd.c_str(), "w");
                if (writer)
                {
                    LOG_INFO("popen success");
                    if (audio_starter)
                    {
                        ((FFAudioEncodeThread *)audio_starter)->START(audio_fifo);
                    }
                    continue;
                }
                LOG_ERROR("popen failed, err=%d:%s, cmd:\n\t%s", errno, strerror(errno), pipe_cmd.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            if (bStopped && writer && pipe_cmd.length())
            {
                LOG_INFO("pclose %s", pipe_cmd.c_str());
                fclose(writer);
                writer = NULL;
                pipe_cmd.clear();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (bExit && videoBuffers.Size()==0)
            break;
        if (!videoBuffers.PopMove(buf, 100)) // empty
        {
            if (bExit) // exit only when queue is empty
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        {
            AUTOTIMED(("Write fifo frame(size: "+std::to_string(buf.size())+") run").c_str(), enable_debug);
            int ret = fwrite(buf.data(), buf.size(), 1, writer);
            if(ret < 0)
            {
                char s[1024];
                strerror_r(errno, s, sizeof(s));
                LOG_ERROR("Error writing to audio fifo for ffmpeg encoding, err=%d:%s", errno, s);
                send_event(ET_PUSH_FAILURE, "write fifo error");
            }
            else
            {
                if(sendnum==0)
                    send_event(ET_START_OF_STREAM, "begin streaming");
                sendnum ++;
            }
        }
    }

    if (writer && pipe_cmd.length()) // it is openned by us, so close it
    {
        fclose(writer);
        writer = NULL;
    }
}

// type: 0 - raw, 1 - video, 2 - audio
int FFVideoEncodeThread::Write(const unsigned char *data, int length, int type, int video_frame_no)
{
    std::string s = type==0? "raw" : (type==1? "video" : "audio");
    AUTOTIMED(("Write queue "+s+" frame run").c_str(), enable_debug);
    static int cnt = 0;
    if(type && cnt++ < 10) // print the first 10 frames
        printf("[rawdata] %s, length: %d\n", type==EC_RAWMEDIA_VIDEO? "video" : "audio", length);
    // ver(int) + type(int) + length(int) + ext_header(video,int)
    int offset = (type==EC_RAWMEDIA_VIDEO? (sizeof(MsgHead)+sizeof(ExtMsgHead)): (type? sizeof(MsgHead) : 0));
    vector<unsigned char> buf(length + offset);
    if (type)
    {
        ((MsgHead* )buf.data())->ver = 1; // version, hardcoded 1
        ((MsgHead* )buf.data())->type = type;
        ((MsgHead* )buf.data())->len = length;
        if (type==EC_RAWMEDIA_VIDEO)
        {
            ((ExtMsgHead *)(buf.data()+sizeof(MsgHead)))->image_index = video_frame_no;
        }
    }
    memcpy(buf.data()+offset, data, length);
    // last = buf;
    videoBuffers.PushMove(std::move(buf));

    // we are going to fast, get some sleep
    if (videoBuffers.Size() > 10)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

void FFAudioEncodeThread::RUN()
{
    while (true)
    {
        if (bStopped)
        {
            if (writer >= 0)
            {
                printf("Closing fifo %s\n", fifo_name.c_str());
                close(writer);
                writer = -1;
            }
            if (bExit) // exit
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (writer < 0)
        {
            if (bExit)
                break;
            LOG_INFO("Openning fifo %s", fifo_name.c_str());
            writer = open(fifo_name.c_str(), O_WRONLY);
            if (writer < 0)
            {
                char s[1024];
                strerror_r(errno, s, sizeof(s));
                LOG_ERROR("Open fifo %s failed, ret=%d:%s", fifo_name.c_str(), errno, s);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            LOG_INFO("Open audio encoding fifo successfully.");
        }
        if (bExit && audioBuffers.Size()==0)
        {
            close(writer);
            writer = -1;
            break;
        }
        vector<unsigned char> buf;
        if (!audioBuffers.PopMove(buf, 100)) // empty
        {
            if (bExit) // exit only when queue is empty
            {
                close(writer);
                writer = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        int ret = write(writer, buf.data(), buf.size());
        if(ret < 0)
        {
            char s[1024];
            strerror_r(errno, s, sizeof(s));
            LOG_ERROR("Error writing to audio fifo for ffmpeg encoding, err=%d:%s", errno, s);
            if (bExit)
                break;
        }
    }
}

void FFSubtitleEncodeThread::RUN()
{
    int ret;
    int frame_number = 0;
    skip_subtitle = false;

    while (true)
    {
        if (bStopped || skip_subtitle || finishedBuffers.Size() >= 5)
        {
            if (bExit) // exit only when queue is empty
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        ret = av_frame_make_writable(frame);
        if (ret)
        {
            skip_subtitle = true;
            LOG_ERROR("Error: av_frame_make_writable failed, ret=%d", ret);
            continue;
        }

        if ((ret=av_image_fill_arrays(frame->data, frame->linesize,
            frameBuffer.data(), AV_PIX_FMT_BGRA, sub_w, sub_h, 1)) < 0)
        {
            skip_subtitle = true;
            LOG_ERROR("Error: av_image_fill_arrays create src image failed, ret=%d", ret);
            continue;
        }
        frame->pts = frame_number ++;

        if ((ret=av_buffersrc_add_frame_flags(buffersrcContext, frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0)
        {
            skip_subtitle = true;
            LOG_ERROR("Error: subtitle filter av_buffersrc_add_frame_flags failed, ret=%d", ret);
            continue;
        }

        while (true)
        {
            ret = av_buffersink_get_frame(buffersinkContext, filterFrame);

            if (ret == AVERROR(EAGAIN))
                break;
            else if (ret == AVERROR_EOF)
            {
                LOG_ERROR("Error: subtitle filter av_buffersink_get_frame returns EOF");
                skip_subtitle = true;
                break;
            }
            else if (ret < 0)
            {
                LOG_ERROR("Error: subtitle filter av_buffersrc_add_frame_flags failed, ret=%d", ret);
                skip_subtitle = true;
                break;
            }

            if (filterFrame->format != AV_PIX_FMT_BGRA) // impossible ?
            {
                LOG_INFO("Warning: av_buffersink_get_frame out fmt(%d) is different from in fmt(AV_PIX_FMT_BGRA-28)", filterFrame->format);
                if (swsContext == NULL)
                {
                    swsContext = sws_getContext(filterFrame->width, filterFrame->height,
                                                (AVPixelFormat)filterFrame->format, sub_w, sub_h,
                                                AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    if (ret < 0)
                    {
                        LOG_ERROR("Error: subtitle filter sws_getContext failed, ret=%d", ret);
                        skip_subtitle = true;
                        av_frame_unref(filterFrame);
                        break;
                    }
                }
                ret = sws_scale(swsContext, filterFrame->data, filterFrame->linesize, 0, 
                                    filterFrame->height, displayFrame->data, displayFrame->linesize);
                if (ret < 0)
                {
                    LOG_ERROR("Error: subtitle filter sws_scale failed, ret=%d", ret);
                    skip_subtitle = true;
                    av_frame_unref(filterFrame);
                    break;
                }
                FFVideoRawData rawdata = {0, (int)filterFrame->pts, std::vector<unsigned char>(buffersize)};
                memcpy(rawdata.frame.data(), displayBuffer.data(), buffersize);
                finishedBuffers.PushMove(std::move(rawdata));
            }
            else
            {
                FFVideoRawData rawdata = {0, (int)filterFrame->pts, std::vector<unsigned char>(buffersize)};
                ret = av_image_copy_to_buffer(rawdata.frame.data(), buffersize,
                            filterFrame->data, filterFrame->linesize,
                            AV_PIX_FMT_BGRA, sub_w, sub_h, 1);
                if (ret < 0)
                {
                    LOG_ERROR("Error: subtitle filter av_image_copy_to_buffer failed, ret=%d", ret);
                    skip_subtitle = true;
                    av_frame_unref(filterFrame);
                    break;
                }
                finishedBuffers.PushMove(std::move(rawdata));
            }

            av_frame_unref(filterFrame);
        }
    }
}

bool FFSubtitleEncodeThread::init_subtitle_filter(
                            AVFilterContext * &buffersrcContext, AVFilterContext * &buffersinkContext,
                            const string &args, const string &filterDesc)
{
    const AVFilter *buffersrc = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *output = avfilter_inout_alloc();
    AVFilterInOut *input = avfilter_inout_alloc();
    AVFilterGraph *filterGraph = avfilter_graph_alloc();

    auto release = [&output, &input] {
        avfilter_inout_free(&output);
        avfilter_inout_free(&input);
    };

    if (!output || !input || !filterGraph) {
        release();
        return false;
    }

    // create grapha filter from args
    int ret;
    if ((ret=avfilter_graph_create_filter(&buffersrcContext, buffersrc, "in",
                                     args.c_str(), nullptr, filterGraph)) < 0) {
        cerr << "Error: avfilter_graph_create_filter for in failed, ret=" << ret << endl;
        release();
        return false;
    }

    if ((ret=avfilter_graph_create_filter(&buffersinkContext, buffersink, "out",
                                     nullptr, nullptr, filterGraph)) < 0) {
        cerr << "Error: avfilter_graph_create_filter for out failed, ret=" << ret << endl;
        release();
        return false;
    }

    output->name = av_strdup("in");
    output->next = nullptr;
    output->pad_idx = 0;
    output->filter_ctx = buffersrcContext;

    input->name = av_strdup("out");
    input->next = nullptr;
    input->pad_idx = 0;
    input->filter_ctx = buffersinkContext;

    if ((ret=avfilter_graph_parse_ptr(filterGraph, filterDesc.c_str(),
                                 &input, &output, nullptr)) < 0) {
        LOG_ERROR("Error: avfilter_graph_parse_ptr for out failed, ret=%d", ret);
        release();
        return false;
    }

    if ((ret=avfilter_graph_config(filterGraph, nullptr)) < 0) {
        LOG_ERROR("Error: avfilter_graph_config for out failed, ret=%d", ret);
        release();
        return false;
    }

    release();
    return true;
}

// replace all occurances of <from> in s to <to>
void replace_all(std::string &s, const std::string &from, const std::string &to)
{
    string::size_type pos = s.find(from), t_size = from.size(), r_size = to.size();
    while(pos != std::string::npos)
    {
        s.replace(pos, t_size, to); 
        pos = s.find(from, pos + r_size ); 
    }
}

int FFSubtitleEncodeThread::START(const std::string &file, const std::string &style,
                int fps, int width, int height, int x, int y, int w, int h)
{
    file_name = file;
    video_fps = fps;
    video_width = width;
    video_height = height;
    sub_x = x; sub_y = y; sub_w = w; sub_h = h;
    bExit.store(false);
    bStopped.store(false);

    if (!file_exists(file))
    {
        LOG_ERROR("Error: subtitle file %s does not exists!", file.c_str());
        return -1;
    }

    string up_style = style;
    for (auto &c : up_style)
        c = toupper(c);
    string force_style = style;
    replace_all(force_style, "%20%", " ");
    replace_all(force_style, "\\&", "&");
    // extract alignment from user style
    const char *token = "ALIGNMENT=";
    char *str = strstr((char*)up_style.c_str(), token);
    if (str)
    {
        align = atoi(str + strlen(token));
    }
    else
    {
        align = 10; // both direction middle
        if (force_style.length())
            force_style += ",";
        force_style += "Alignment=" + std::to_string(align);
    }
    force_style += ",WrapStyle=1"; // wrap line allowing not equal length

    int vert_align = align / 4; // 0 bottom, 1 top, 2 middle
    int hori_align = align % 4; // 1 left, 2 middle, 3 right

    int max_height = height * 2;
    if (max_height < h * 2)
        max_height = h * 2;

    if (vert_align == 0) // bottom
    {
        sub_y = y + h - height;
        sub_h = height; // y + h;
        if (sub_h > max_height) // limit the height, keep the bottom pos
        {
            sub_h = max_height;
            sub_y = y + h - max_height;
        }
    }
    else if (vert_align == 1) // top
    {
        sub_y = y;
        sub_h = height; // height - y;
        if (sub_h > max_height) // limit the height, keep the top pos
        {
            sub_h = max_height;
        }
    }
    else // middle
    {
        int center_y = y + h / 2;
        sub_h = height; // fix height to video height
        sub_y = center_y - sub_h / 2;
#if 0
        if (center_y > height/2) // lower part
        {
            sub_h = center_y * 2;
            sub_y = 0;
        }
        else // upper part
        {
            sub_h = (height - center_y) * 2;
            sub_y = center_y - sub_h / 2;
        }
        if (sub_h > max_height) // limit the height, keep the center pos
        {
            sub_h = max_height;
            sub_y = center_y - sub_h / 2;
        }
#endif
    }

    if (sub_w < MIN_SUBTITLE_WIDTH)
    {
        // if out of width, shrink x, so that it may have more visible space
        if (sub_x + sub_w > width)
            sub_x = (sub_x + sub_w) - MIN_SUBTITLE_WIDTH;
        sub_w = MIN_SUBTITLE_WIDTH;
    }

    char args[1024];
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", // sub_w > sub_h? 16 : 9, sub_w > sub_h? 9 : 16
                                    sub_w, sub_h, AV_PIX_FMT_BGRA, 1, fps, sub_w, sub_h);
    cout << "Subtitle video args: " << args << endl;

    // make sure original_size area is same is original size
    int area = width * height;
    int sub_area = sub_w * sub_h;
    float ratio = ((float)area) / sub_area;
    int vir_w = sub_w * ratio;
    int vir_h = sub_h * ratio;

    char filterDesc[4096];
    snprintf(filterDesc, sizeof(filterDesc)-1, "subtitles=filename='%s':original_size=%dx%d:alpha=1", file.c_str(), vir_w, vir_h);
    if (force_style.length())
    {
        replace_all(force_style, ",", "\\,"); // , will be treated as option separate token
        int len = strlen(filterDesc);
        snprintf(filterDesc+len, sizeof(filterDesc)-1-len, ":force_style=‘%s'", force_style.c_str());
    }
    filterDesc[sizeof(filterDesc)-1] = 0;
    LOG_INFO("Subtitle filter args: %s", filterDesc);

    // init subtitle filter
    buffersrcContext = nullptr;
    buffersinkContext = nullptr;
    bool subtitleOpened = init_subtitle_filter(buffersrcContext, buffersinkContext, args, filterDesc);
    if (!subtitleOpened)
        return -1;

    int ret;
    // init frame and sws context
    frame = av_frame_alloc();
    displayFrame = av_frame_alloc();
    filterFrame = av_frame_alloc();
    buffersize = av_image_get_buffer_size(AV_PIX_FMT_BGRA, sub_w, sub_h, 1);

    frame->width = sub_w;
    frame->height = sub_h;
    frame->format = AV_PIX_FMT_BGRA;
    frameBuffer.resize(buffersize);
    memset(frameBuffer.data(), 0, buffersize);
    ret = av_frame_get_buffer(frame, 0);
    if (ret)
    {
        av_frame_free(&frame);
        av_frame_free(&displayFrame);
        av_frame_free(&filterFrame);
        LOG_ERROR("Error: av_frame_get_buffer failed, ret=%d", ret);
        return -1;
    }

    displayBuffer.resize(buffersize);
    if ((ret=av_image_fill_arrays(displayFrame->data, displayFrame->linesize,
    	displayBuffer.data(), AV_PIX_FMT_BGRA, sub_w, sub_h, 1)) < 0)
    {
        av_frame_free(&frame);
        av_frame_free(&displayFrame);
        av_frame_free(&filterFrame);
        LOG_ERROR("Error: av_image_fill_arrays create dst image failed, ret=%d", ret);
        return -1;
    }

    // start thread
    if (runner == NULL)
        runner = new std::thread(&FFSubtitleEncodeThread::RUN, this);
    return 0;
}

