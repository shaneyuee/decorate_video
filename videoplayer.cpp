#include <vector>
#include <string>
#include <thread>
#include <opencv2/opencv.hpp>
#include "videoplayer.h"
#include "AutoTime.h"
#include "decorateVideo.h"
#include "safequeue.h"
#include "event.h"
#include "material.h"
#include "3rd/shmqueue/shm_queue.h"
#include "3rd/log/LOGHelp.h"

#undef	__MODULE__
#define __MODULE__ "VideoPlayer"

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/eval.h>
#include <libavutil/display.h>
}

extern int enable_debug;

#define STAT_RUNTIME enable_debug

#define AUDIO_DELTA  64

int64_t get_video_duration(const char *file)
{
    AVFormatContext* formatCtx = NULL;
    int ret;
    int64_t duration_ms;

    if ((ret=avformat_open_input(&formatCtx, file, nullptr, nullptr)) < 0)
    {
        LOG_ERROR("failed to open file %s", file);
        return -1;
    }

    if ((ret=avformat_find_stream_info(formatCtx, nullptr)) < 0)
    {
        avformat_close_input(&formatCtx);
        LOG_ERROR("failed to get stream info for %s", file);
        return -1;
    }
    if (formatCtx->duration != AV_NOPTS_VALUE)
        duration_ms = (formatCtx->duration + 5000) / 1000;
    else
        duration_ms = 0;

    avformat_close_input(&formatCtx);
    return duration_ms;
}

bool check_webm_has_alpha(const char *file)
{
    std::string s = "ffmpeg -vcodec libvpx-vp9 -i " + std::string(file) + " 2>&1|grep 'yuva420p'";
    int ret = system(s.c_str());
    if (ret == 0)
        return true;
    return false;
}

double get_rotation(AVStream *st)
{
    AVDictionaryEntry *rotate_tag = av_dict_get(st->metadata, "rotate", NULL, 0);
    uint8_t* displaymatrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, NULL);
    double theta = 0;

    if (rotate_tag && *rotate_tag->value && strcmp(rotate_tag->value, "0")) {
        char *tail;
        theta = av_strtod(rotate_tag->value, &tail);
        if (*tail)
            theta = 0;
    }
    if (displaymatrix && !theta)
        theta = -av_display_rotation_get((int32_t*) displaymatrix);

    theta -= 360*floor(theta/360 + 0.9/360);

    if (fabs(theta - 90*round(theta/90)) > 2)
        av_log(NULL, AV_LOG_WARNING, "Odd rotation angle.\n"
                "If you want to help, upload a sample "
                "of this file to ftp://upload.ffmpeg.org/incoming/ "
                "and contact the ffmpeg-devel mailing list. (ffmpeg-devel@ffmpeg.org)");

    return theta;
}

void mverge_audio(std::vector<pcm_info> &pcms, int pcm_length, std::vector<unsigned char> &out)
{
    out.resize(pcm_length*2);
    if(pcms.size()==0)
    {
        memset(out.data(), 0, pcm_length*2);
        return;
    }
    if(pcms.size()==1)
    {
        int len = pcm_length < pcms[0].length? pcm_length : pcms[0].length;
        memcpy(out.data(), pcms[0].pcm, len*2);
        if(pcm_length > len)
            memset(out.data()+len*2, 0, (pcm_length-len)*2);
        return;
    }

    int all_volum = 0;
    for (auto &pcm : pcms)
        all_volum += pcm.volum;
    if (all_volum <= 0)
    {
        memset(out.data(), 0, pcm_length*2);
        return;
    }

    for(int i=0; i<pcm_length; i++)
    {
        int all = 0;
        for (auto &pcm : pcms)
        {
            if(i >= pcm.length)
                continue;
            all += ((int)pcm.pcm[i]) * pcm.volum;
        }
        ((short *)out.data())[i] = all / all_volum;
    }
}

static void read_video_close_only(FFReader *video)
{
    video->buffers.clear();
    video->rawBuffer.clear();
    video->rawAudio.clear();
    video->audio_size = 0;
    if(video->swsCtx)
    {
        sws_freeContext(video->swsCtx);
        video->swsCtx = NULL;
    }
    if(video->displayFrame)
        av_frame_free(&video->displayFrame);
    if(video->frame)
        av_frame_free(&video->frame);
    if(video->avCodecCtx)
        avcodec_free_context(&video->avCodecCtx);
    if(video->formatCtx)
        avformat_close_input(&video->formatCtx);
    if(video->avAudioCodecCtx)
        avcodec_free_context(&video->avAudioCodecCtx);
    if(video->audioFrame)
        av_frame_free(&video->audioFrame);
    if(video->m_swrCtx)
        swr_free(&video->m_swrCtx);
    if(video->fraw && video->fraw != stdin)
    {
        fclose(video->fraw);
        video->fraw = NULL;
    }
    if(video->fshm)
    {
        sq_destroy_and_remove(video->fshm);
        video->fshm = NULL;
    }
    video->video_pts_time = -1.0;
    video->audio_pts_time = -1.0;
}

int open_video_reader(FFReader *video)
{
    if (video->fraw || video->fshm || video->formatCtx) // already opened
        return 0;

    AVFormatContext* &formatCtx = video->formatCtx;
    AVFrame* &frame = video->frame;
    AVFrame* &displayFrame = video->displayFrame;
    AVCodecContext* &avCodecCtx = video->avCodecCtx;
    SwsContext* &swsCtx = video->swsCtx;

    int error = 0;
    video->rotation = 0.0;
    bool rotation_90 = false;

    do {
        int ret = avformat_open_input(&formatCtx, video->filename.c_str(), nullptr, nullptr);
        if (ret < 0)
        {
            char str[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, str, AV_ERROR_MAX_STRING_SIZE);
            LOG_ERROR("avformat_open_input failed, error %d:%s, video path:%s", ret, str, video->filename.c_str());
            error = -1;
            break;
        }

        if (avformat_find_stream_info(formatCtx, nullptr) < 0)
        {
            LOG_ERROR("avformat_find_stream_info failed, video path %s.", video->filename.c_str());
            error = -1;
            break;
        }
        if (formatCtx->duration != AV_NOPTS_VALUE)
            video->totaltime = (formatCtx->duration + 5000) / 1000;
        else
            video->totaltime = 0;

        if(video->decode_video)
        {
            ret = av_find_best_stream(formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
            if (ret < 0)
            {
                LOG_ERROR("av_find_best_stream not found video stream, path:%s", video->filename.c_str());
                error = -1;
                break;
            }
            video->streamIndex = ret;
            // codec parameters
            AVCodecParameters *codecParameters = formatCtx->streams[video->streamIndex]->codecpar;
            if (!codecParameters)
            {
                LOG_ERROR("Not found codec parameters for video:%s", video->filename.c_str());
                error = -1;
                break;
            }
            video->rotation = get_rotation(formatCtx->streams[video->streamIndex]);
            while (video->rotation < -0.1)
                video->rotation += 360.0;
            while (video->rotation > 360.0)
                video->rotation -= 360.0;
            if (video->rotation > 0.1)
            {
                LOG_INFO("Video has rotation of %f degree, video: %s", video->rotation, video->filename.c_str());
                if (fabs(video->rotation - 90.0) < 1.0 || fabs(video->rotation - 270.0) < 1.0)
                    rotation_90 = true;
            }
            AVCodec *avCode = (AVCodec *)avcodec_find_decoder(codecParameters->codec_id);
            if(codecParameters->codec_id==AV_CODEC_ID_VP9)
            {
                // built-in vp8/vp9 decoders do not extract alpha, use libvpx instread
                AVCodec* pDecoder2 = (AVCodec *)avcodec_find_decoder_by_name("libvpx-vp9");
                LOG_ERROR("Using libvpx-vp9 for vp9 decoding, decoder=%p.", pDecoder2);
                if(pDecoder2) avCode = pDecoder2;
            }

            AVHWDeviceType tt = av_hwdevice_find_type_by_name(avCode->name);
            LOG_INFO("Found AV codec %s(longname:%s, hwtype:%d) for video.", avCode->name, avCode->long_name, tt);
            if (!avCode)
            {
                LOG_ERROR("Not found decoder for video:%s", video->filename.c_str());
                error = -1;
                break;
            }
            // get decoder
            avCodecCtx = avcodec_alloc_context3(avCode);
            avCodecCtx->thread_count = 16;
            if (avcodec_parameters_to_context(avCodecCtx, codecParameters) != 0)
            {
                LOG_ERROR("avcodec_parameters_to_context failed, path:%s", video->filename.c_str());
                error = -1;
                break;
            }
            if(strcmp(avCode->name, "libvpx-vp9")==0)
            {
                if(check_webm_has_alpha(video->filename.c_str()))
                {
                    LOG_INFO("Webm file %s has alpha channel, force to yuva420p format.", video->filename.c_str());
                    avCodecCtx->pix_fmt = AV_PIX_FMT_YUVA420P;
                }
            }
            // open decoder
            ret = avcodec_open2(avCodecCtx, avCode, nullptr);
            if (ret < 0)
            {
                LOG_ERROR("avcodec_open2 failed. result=(%d) path:%s", ret, video->filename.c_str());
                error = -1;
                break;
            }
            video->pix_fmt = avCodecCtx->pix_fmt;
            video->width = avCodecCtx->width;
            video->height = avCodecCtx->height;
            if (video->disp_width > 0 && video->disp_height > 0)
            {
                if (rotation_90) // swap width & height
                {
                    int w = video->disp_width;
                    video->disp_width = video->disp_height;
                    video->disp_height = w;
                }
            }
            else
            {
                video->disp_width = video->width;
                video->disp_height = video->height;
            }
            double tb = av_q2d(formatCtx->streams[video->streamIndex]->time_base);
            if(tb > 0.01f)
            {
                video->fps = 1 / tb;
                LOG_INFO("Using time_base %f for fps %d.", tb, (int)(video->fps+0.5));
            }
            else
            {
                double fr = av_q2d(formatCtx->streams[video->streamIndex]->r_frame_rate);
                LOG_INFO("Using frame_rate %f for fps %d, timebase=%f.", fr, (int)(fr+0.5), tb);
                video->fps = fr;
            }
            if (video->fps > 60.0) // invalid fps, default to 30
            {
                LOG_ERROR("Invalid fps %f for %s, default to 30", video->fps, video->filename.c_str());
                video->fps = 30.0;
            }
            video->video_timebase = tb;
            video->bitrate = formatCtx->bit_rate;
            frame = av_frame_alloc();
            // scale image output
            displayFrame = av_frame_alloc();
            video->buffersize = av_image_get_buffer_size(video->out_pix_fmt, video->disp_width, video->disp_height, 1);
            video->displayBuffer.resize(video->buffersize);
            if (av_image_fill_arrays(displayFrame->data, displayFrame->linesize,
                video->displayBuffer.data(), video->out_pix_fmt, video->disp_width, video->disp_height, 1) < 0)
            {
                fprintf(stderr, "av_image_fill_arrays create dst image failed, path:%s\n", video->filename.c_str());
                error = -1;
                break;
            }
            displayFrame->width = video->disp_width;
            displayFrame->height = video->disp_height;
            int flag = SWS_BILINEAR; // scale_prefer default is both
            if (video->scale_prefer && strncasecmp(video->scale_prefer, "quality", 7)==0)
                flag = video->width > video->disp_width ? SWS_BICUBIC : SWS_BICUBLIN;
            else if (video->scale_prefer && strncasecmp(video->scale_prefer, "speed", 5)==0)
                flag = SWS_FAST_BILINEAR;
            else if (video->scale_prefer && strncasecmp(video->scale_prefer, "both", 4)==0)
                flag = SWS_BILINEAR;
            swsCtx = sws_getContext(video->width, video->height, avCodecCtx->pix_fmt,
                video->disp_width, video->disp_height, video->out_pix_fmt, flag, NULL, NULL, NULL);
            if (!swsCtx)
            {
                LOG_ERROR("swsCtx create fail path:%s", video->filename.c_str());
                error = -1;
                break;
            }
            LOG_INFO("Openned video %s with size [%dx%d], output size [%dx%d]", video->filename.c_str(),
                     video->width, video->height, video->disp_width, video->disp_height);
        }

        if(video->decode_audio)
        {
            // Init audio decoder parameters
            ret = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
            if (ret < 0)
            {
                LOG_INFO("Warning: av_find_best_stream not found audio stream, path:%s, disable audio", video->filename.c_str());
                error = 0;
                video->decode_audio = false;
                break;
            }
            video->audioStreamIndex = ret;
            AVCodecParameters *audioCodecParameters = formatCtx->streams[video->audioStreamIndex]->codecpar;
            if (!audioCodecParameters)
            {
                fprintf(stderr, "Not found codec parameters for audio:%s\n", video->filename.c_str());
                error = -1;
                break;
            }
            video->avAudioCodecParam = audioCodecParameters;
            AVCodec *avAudioCode = (AVCodec *)avcodec_find_decoder(audioCodecParameters->codec_id);

            // retrieve audio decoder
            video->avAudioCodecCtx = avcodec_alloc_context3(avAudioCode);
            video->avAudioCodecCtx->thread_count = 16;
            if ((ret=avcodec_parameters_to_context(video->avAudioCodecCtx, audioCodecParameters)) != 0)
            {
                LOG_ERROR("avcodec_parameters_to_context(AUDIO) failed, ret=%d", ret);
                error = -1;
                break;
            }
            // open audio decoder
            ret = avcodec_open2(video->avAudioCodecCtx, avAudioCode, nullptr);
            if (ret < 0)
            {
                LOG_ERROR("avcodec_open2(AUDIO) failed, ret=%d", ret);
                error = -1;
                break;
            }
            auto str = formatCtx->streams[video->audioStreamIndex];
            auto tb = av_q2d(str->time_base);
            if (tb > 0.00000001f)
                video->audio_timebase = tb;
            else
            {
                auto fps = av_q2d(str->r_frame_rate);
                if (fps < 1.0f)
                    video->audio_timebase = video->video_timebase;
                else
                    video->audio_timebase = 1.0 / fps;
            }
            video->audioFrame = av_frame_alloc();
            video->m_swrCtx = swr_alloc_set_opts(nullptr, av_get_default_channel_layout(video->aud_channel), 
                    video->aud_samplefmt, video->aud_bitrate,
                    av_get_default_channel_layout(video->avAudioCodecCtx->channels), video->avAudioCodecCtx->sample_fmt,
                    video->avAudioCodecCtx->sample_rate, 0, nullptr);
            if (!video->m_swrCtx || (ret=swr_init(video->m_swrCtx)) < 0)
            {
                LOG_ERROR("swr_init failed, err=%d", ret);
                swr_free(&video->m_swrCtx);
                error = -1;
                break;
            }
            video->audio_size = 0;
        }
    }while(0);

    if(error)
    {
        read_video_close_only(video);
    }
    return error;
}


int reopen_video_reader(FFReader *video)
{
    if (video->fraw || video->fshm) // raw video does not support reopening
        return 0;
    if (video->formatCtx)
        read_video_close_only(video);
    return open_video_reader(video);
}

FFReader *new_video_reader(const char *filename, bool decode_video, bool decode_audio,
                            int width, int height, const char *scale_prefer, AVPixelFormat out_pixfmt,
                            AVSampleFormat out_audfmt, int out_channel, int64_t out_samplerate)
{
    FFReader *video = new FFReader();
    video->fraw = NULL;
    video->fshm = NULL;
    video->filename = filename;
    video->out_pix_fmt = out_pixfmt;
    video->scale_prefer = scale_prefer;
    video->disp_width = width;
    video->disp_height = height;
    video->decode_video = decode_video;
    video->decode_audio = decode_audio;
    video->streamIndex = -1;
    video->audioStreamIndex = -1;
    video->colorRange = video->colorSpace = 0;
    video->update_time.store(0);
    video->audio_size = 0;
    video->video_pts_time = -1.0;
    video->audio_pts_time = -1.0;
    video->audio_adjusted = false;
    video->rotation = 0.0;

    if(!decode_audio && !decode_video)
    {
        LOG_ERROR("Invalid parameters, both decode_audio and decode_video are false!");
        delete video;
        return NULL;
    }

    video->aud_fmt = (char*)"pcm_s16le";
    video->aud_samplefmt = out_audfmt;
    video->aud_channel = out_channel;
    video->aud_bitrate = out_samplerate;
    return video;
}

FFReader *open_video_reader(const char *filename, bool decode_video, bool decode_audio,
                            int width, int height, const char *scale_prefer, AVPixelFormat out_pixfmt,
                            AVSampleFormat out_audfmt, int out_channel, int64_t out_samplerate)
{
    FFReader *video = new_video_reader(filename, decode_video, decode_audio, width,
                            height, scale_prefer, out_pixfmt, out_audfmt, out_channel, out_samplerate);
    if(video == NULL)
        return NULL;

    int error = open_video_reader(video);
    if (error == 0)
        return video;

    delete video;
    return NULL;
}

int parse_pix_fmt(const char *pix_fmt)
{
    if (pix_fmt == NULL)
        return AV_PIX_FMT_NONE;
    if (strncasecmp(pix_fmt, "bgra", 5)==0)
        return AV_PIX_FMT_BGRA;
    if (strncasecmp(pix_fmt, "bgr", 4)==0)
        return AV_PIX_FMT_BGR24;
    if (strncasecmp(pix_fmt, "rgba", 5)==0)
        return AV_PIX_FMT_RGBA;
    if (strncasecmp(pix_fmt, "rgb", 4)==0)
        return AV_PIX_FMT_RGB24;
    if (strncasecmp(pix_fmt, "yuv420p", 8)==0)
        return AV_PIX_FMT_YUV420P;
    return AV_PIX_FMT_NONE;
}

FFReader *open_raw_video(const char *filename, int disp_width, int disp_height, const char *scale_prefer,
                        const char *pix_fmt, const char *out_pix_fmt, int width, int height, float fps, int64_t bitrate,
                        const char *aud_fmt, int aud_channel, int64_t aud_bitrate)
{
    int fmt = parse_pix_fmt(pix_fmt);
    int out_fmt = parse_pix_fmt(out_pix_fmt);
    if ((pix_fmt && fmt < 0) || (out_pix_fmt && out_fmt < 0))
    {
        LOG_ERROR("invalid pix_fmt: %s, out_pix_fmt: %s", pix_fmt, out_pix_fmt);
        return NULL;
    }
    if (!out_fmt)
        pix_fmt = NULL;
    if (!pix_fmt && !aud_fmt)
    {
        LOG_ERROR("invalid pix_fmt and aud_fmt, must specify at least one.");
        return NULL;
    }

    FFReader *video = new FFReader();
    video->decode_video = pix_fmt!=NULL;
    video->decode_audio = aud_fmt!=NULL;
    video->fps = fps;
    video->rotation = 0.0;
    video->pix_fmt = (AVPixelFormat)fmt;
    video->out_pix_fmt = (AVPixelFormat)out_fmt;
    video->disp_height = disp_height? disp_height : height;
    video->disp_width = disp_width? disp_width : width;
    video->width = width;
    video->height = height;
    video->bitrate = bitrate;
    video->aud_fmt = (char*)aud_fmt;
    video->aud_bitrate = aud_bitrate;
    video->aud_channel = aud_channel;
    video->product_id = 0;
    video->update_time.store(0);
    video->audio_size = 0;
    video->video_pts_time = -1.0;
    video->audio_pts_time = -1.0;
    video->audio_adjusted = false;

    if (video->decode_video)
    {
        video->frame = av_frame_alloc();
        video->displayFrame = av_frame_alloc();
        video->buffersize = av_image_get_buffer_size(video->out_pix_fmt, video->disp_width, video->disp_height, 1);
        video->framesize = av_image_get_buffer_size(video->pix_fmt, video->width, video->height, 1);
        video->rawBuffer.resize(video->framesize);
        video->displayBuffer.resize(video->buffersize);
        LOG_INFO("rawvideo input:  w=%d, h=%d, framesize=%d, color=%s", video->width, video->height, video->framesize, pix_fmt);
        LOG_INFO("rawvideo output: w=%d, h=%d, framesize=%d, color=%s", video->disp_width, video->disp_height, video->buffersize, out_pix_fmt);
    }
    else
    {
        video->frame = NULL;
        video->displayFrame = NULL;
        video->buffersize = 0;
        video->framesize = 0;
    }
    if(aud_fmt)
        video->rawAudio.resize(1024*1024);
    if (video->decode_video)
    {
        if (av_image_fill_arrays(video->displayFrame->data, video->displayFrame->linesize,
            video->displayBuffer.data(), video->out_pix_fmt, video->disp_width, video->disp_height, 1) < 0)
        {
            av_frame_free(&video->frame);
            av_frame_free(&video->displayFrame);
            delete video;
            LOG_ERROR("av_image_fill_arrays create dst image failed, path:%s", filename);
            return NULL;
        }
        video->displayFrame->width = video->disp_width;
        video->displayFrame->height = video->disp_height;
        video->frame->width = video->width;
        video->frame->height = video->height;
        int flag = SWS_BILINEAR; // scale_prefer default is both
        if (scale_prefer && strncasecmp(scale_prefer, "quality", 7)==0)
            flag = video->width > video->disp_width ? SWS_BICUBIC : SWS_BICUBLIN;
        else if (scale_prefer && strncasecmp(scale_prefer, "speed", 5)==0)
            flag = SWS_FAST_BILINEAR;
        else if (scale_prefer && strncasecmp(scale_prefer, "both", 4)==0)
            flag = SWS_BILINEAR;
        video->swsCtx = sws_getContext(video->width, video->height, video->pix_fmt,
            video->disp_width, video->disp_height, video->out_pix_fmt, flag, NULL, NULL, NULL);
        if (!video->swsCtx)
        {
            av_frame_free(&video->frame);
            av_frame_free(&video->displayFrame);
            delete video;
            LOG_ERROR("swsCtx create fail path:%s", filename);
            return NULL;
        }
    }

    if (strncmp(filename, "-", 2)==0)
    {
        video->fraw = stdin;
    }
    else if (strncmp(filename, "shm://", 6)==0)
    {
        video->fshm = sq_open_by_shmid(atoi(filename+6));
    }
    else
    {
        video->fraw = fopen(filename, "rb");
    }
    if (video->fraw==NULL && video->fshm==NULL)
    {
        if (video->decode_video)
        {
            av_frame_free(&video->frame);
            av_frame_free(&video->displayFrame);
            sws_freeContext(video->swsCtx);
        }
        delete video;
        if (strncmp(filename, "shm://", 6)==0)
            LOG_ERROR("Failed to open rawvideo by shmid %d, error=%d:%s", atoi(filename+6), errno, sq_errorstr(video->fshm));
        else
            LOG_ERROR("Failed to open rawvideo file:%s, error=%d:%s", filename, errno, strerror(errno));
        return NULL;
    }
    video->filename = filename;

    return video;
}

static const int AudioBufferSize = 192000;

int decode_audio_frame(FFReader *video, AVPacket *packet)
{
    int ret = avcodec_send_packet(video->avAudioCodecCtx, packet);
    if (ret < 0)
    {
        LOG_ERROR("avcodec_send_packet(AUDIO) failed, ret=%d(%s)", ret, ret==AVERROR_EOF? "EOF":"Unknown");
    }
    while (ret==0 && (ret = avcodec_receive_frame(video->avAudioCodecCtx, video->audioFrame)) >= 0)
    {
        AVFrame *frame = video->audioFrame;
        const uint8_t **indata = (const uint8_t**)frame->extended_data;
        static uint8_t *staticdata = new uint8_t[AudioBufferSize];
        uint8_t *outdata = staticdata;
        int outSample = swr_get_out_samples(video->m_swrCtx, frame->nb_samples);// frame->nb_samples * video->aud_bitrate / frame->sample_rate + 256;
        int nbsamples = swr_convert(video->m_swrCtx, &outdata, outSample, indata, frame->nb_samples);
        if(nbsamples == 0)
        {
            LOG_ERROR("Warning: swr_convert output 0 samples, input is %d", outSample);
            continue;
        }
        if(nbsamples < 0)
        {
            ret = nbsamples;
            LOG_ERROR("Error: swr_convert failed, ret=%d", nbsamples);
            break;
        }
        int bytes = av_samples_get_buffer_size(NULL, video->aud_channel, nbsamples, AV_SAMPLE_FMT_S16, 1);
        if(bytes + video->audio_size > video->rawAudio.size())
        {
            if(video->rawAudio.size() > 10*1024*1024)
            {
                LOG_ERROR("Warning: audio buffer size (%d) exceeds max of 10M, reset to 0", (int)video->rawAudio.size());
                video->audio_size = 0;
            }
            else
            {
                LOG_ERROR("Warning: increasing audio buffer size to %d", (int)video->rawAudio.size()+1024*1024);
                video->rawAudio.resize(video->rawAudio.size()+1024*1024);
            }
        }
        memcpy(video->rawAudio.data()+video->audio_size, outdata, bytes);
        video->audio_size += bytes;
        video->audio_pts_time = video->audioFrame->pts * video->audio_timebase;
        LOG_DEBUG("recv audio, packet_dts=%lld, packet_pts=%lld, packet_pos=%lld, frame_dts=%lld, frame_pts=%lld, frame_pos=%lld, pts_time=%f, length=%d", packet->dts, packet->pts, packet->pos, video->audioFrame->pkt_dts, video->audioFrame->pts, video->audioFrame->pkt_pos, video->audio_pts_time, bytes);

        // release src frame
        av_frame_unref(frame);
    }
    if(ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
    {
        char str[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, str, AV_ERROR_MAX_STRING_SIZE);
        LOG_ERROR("Error: failed to decode audio packet, error code: %d:%s", ret, str);
    }
    return ret;
}

int decode_video_frame(FFReader *video, AVPacket *packet, std::vector<uint8_t> **video_buffer)
{
    int error = 0;
    int ret = avcodec_send_packet(video->avCodecCtx, packet);
    if (ret < 0 )
    {
        LOG_ERROR("avcodec_send_packet(VIDEO) failed, ret=%d(%s)", ret, ret==AVERROR_EOF? "EOF":"Unknown");
    }
    while (ret >= 0 && (ret=avcodec_receive_frame(video->avCodecCtx, video->frame)) >= 0)
    {
        video->video_pts_time = video->frame->pts * video->video_timebase;
        LOG_DEBUG("recv video, packet_dts=%lld, packet_pts=%lld, packet_pos=%lld, frame_dts=%lld, frame_pts=%lld, frame_pos=%lld, pts_time=%f", packet->dts, packet->pts, packet->pos, video->frame->pkt_dts, video->frame->pts, packet->pos, video->video_pts_time);
        if (video_buffer && *video_buffer == &video->displayBuffer) // duplicate for reuse
        {
            video->rawBuffer.resize(video->displayBuffer.size());
            memcpy(video->rawBuffer.data(), video->displayBuffer.data(), video->displayBuffer.size());
            *video_buffer = &video->rawBuffer;
        }
        // read video
        if ((video->width % 8) && video->frame->format==AV_PIX_FMT_YUV420P && // 不是8对齐，ffmpeg读出BGR会有黑边，自己转
                (video->out_pix_fmt==AV_PIX_FMT_BGR24 || video->out_pix_fmt==AV_PIX_FMT_BGRA))
        {
            cv::Mat m1(cv::Size(video->width, video->height*3/2), CV_8U);
            ret = av_image_copy_to_buffer(m1.data, video->width*video->height*3/2,
                        video->frame->data, video->frame->linesize,
                        AV_PIX_FMT_YUV420P, video->width, video->height, 1);
            if (ret < 0)
            {
                av_frame_unref(video->frame);
                LOG_ERROR("av_image_copy_to_buffer failed");
                error = -1;
                break;
            }
            int dst_type = video->out_pix_fmt==AV_PIX_FMT_BGRA? CV_8UC4 : CV_8UC3;
            int dst_color = video->out_pix_fmt==AV_PIX_FMT_BGRA? cv::COLOR_YUV2BGRA_I420 : cv::COLOR_YUV2BGR_I420;
            if (video->disp_width==video->width && video->disp_height==video->height)
            {
                cv::Mat m2(cv::Size(video->width, video->height), dst_type, video->displayBuffer.data());
                cv::cvtColor(m1, m2, dst_color);
            }
            else
            {
                cv::Mat m2(cv::Size(video->width, video->height), dst_type);
                cv::Mat m3(cv::Size(video->disp_width, video->disp_height), dst_type, video->displayBuffer.data());
                cv::cvtColor(m1, m2, dst_color);
                cv::resize(m2, m3, cv::Size(video->disp_width, video->disp_height));
            }
        }
        else
        {
#ifdef FFMPEG_DECODE_COLORSPACE
            if (video->frame->color_range != video->colorRange || video->frame->colorspace != video->colorSpace)
            {
                int srcRange = 0;
                if (video->frame->color_range == AVCOL_RANGE_JPEG)
                    srcRange = 1;
                int srcCS = SWS_CS_DEFAULT;
                if (video->frame->colorspace == AVCOL_SPC_BT709)
                    srcCS = SWS_CS_ITU709;
                else if (video->frame->colorspace == AVCOL_SPC_FCC)
                    srcCS = SWS_CS_FCC;
                else if (video->frame->colorspace == AVCOL_SPC_SMPTE240M)
                    srcCS = SWS_CS_SMPTE240M;
                else if (video->frame->colorspace == AVCOL_SPC_BT2020_CL || video->frame->colorspace == AVCOL_SPC_BT2020_NCL)
                    srcCS = SWS_CS_BT2020;
                else if (video->frame->colorspace == AVCOL_SPC_UNSPECIFIED)
                {
                    if (video->frame->height > 576)
                        srcCS = SWS_CS_ITU709;
                }
                sws_setColorspaceDetails(video->swsCtx, sws_getCoefficients(srcCS), srcRange,
                    sws_getCoefficients(SWS_CS_DEFAULT), 0, 0, 1 << 16, 1 << 16);
                video->colorRange = video->frame->color_range;
                video->colorSpace = video->frame->colorspace;
            }
#endif
            ret = sws_scale(video->swsCtx, video->frame->data,
                video->frame->linesize, 0, video->frame->height, video->displayFrame->data, video->displayFrame->linesize);
            if (ret < 0)
            {
                av_frame_unref(video->frame);
                LOG_ERROR("sws_scale failed");
                error = -1;
                break;
            }
        }

        if(video_buffer && *video_buffer == NULL)
        {
            *video_buffer = &video->displayBuffer;
        }
        else
        {
            std::vector<unsigned char> r(video->displayBuffer.size());
            memcpy(r.data(), video->displayBuffer.data(), video->displayBuffer.size());
            video->buffers.push_back(std::move(r));
        }

        // 释放src frame
        av_frame_unref(video->frame);
    }
    if(ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
    {
        char str[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, str, AV_ERROR_MAX_STRING_SIZE);
        LOG_ERROR("Error: failed to decode packet, error code: %d:%s", ret, str);
    }
    return ret;
}



int read_rawvideo_frame(FFReader *video, std::vector<unsigned char> **buffer)
{
    *buffer = NULL;
    MsgHead header = { 0 };
    ExtMsgHeadExtAudio extaudioheader = { 0 };

    while(video->aud_fmt) // buffer all raw audio at video->rawAudio until one video frame is encountered
    {
        {
        AUTOTIMED(("[rawvideo] read header, size: "+std::to_string(sizeof(header))).c_str(), STAT_RUNTIME);
        if (fread(&header, sizeof(header), 1, video->fraw) != 1)
        {
            if (feof(video->fraw))
            {
                LOG_INFO("eof occur.");
                return 0;
            }
            LOG_ERROR("Failed to read rawvideo header, err=%d:%s", errno, strerror(errno));
            return -1;
        }
        }
        if (header.ver != 1 || (header.type != EC_RAWMEDIA_VIDEO &&
            header.type != EC_RAWMEDIA_AUDIO && header.type != EC_RAWMEDIA_EXT_AUDIO))
        {
            LOG_ERROR("Error: bad header in rawvideo, type %d, version %d!", header.type, header.ver);
            return -1;
        }
        if (header.type == EC_RAWMEDIA_VIDEO)
        {
            if (header.len != video->framesize) // video
            {
                LOG_ERROR("Error: bad header video length, expect %d, got %d!", video->framesize, header.len);
                return -1;
            }
            break;
        }
        extaudioheader.product_id = 0;
        if (header.type == EC_RAWMEDIA_EXT_AUDIO)
        {
            {
            AUTOTIMED("[rawvideo] read ext audio header, size: 4", STAT_RUNTIME);
            if (fread(&extaudioheader, sizeof(extaudioheader), 1, video->fraw) != 1)
            {
                if (feof(video->fraw))
                {
                    LOG_INFO("eof occur.");
                    break;
                }
                LOG_ERROR("Failed to read rawvideo ext audio header, err=%d:%s", errno, strerror(errno));
                return -1;
            }
            }
        }
        if (extaudioheader.product_id != video->product_id) // switch product, clear buffer
        {
            LOG_INFO("Switching product from %d to %d", video->product_id, extaudioheader.product_id);
            video->product_id = extaudioheader.product_id;
            video->audio_size = 0;
        }
        if (header.len <= 0 || header.len > 1024*1024)
        {
            LOG_ERROR("Error: bad header audio length of %d (max allowed is 1M)!", header.len);
            return -1;
        }
        if(header.len + video->audio_size > video->rawAudio.size())
        {
            LOG_INFO("Warning: increasing rawaudio buffer size to %d", (int)video->rawAudio.size()+1024*1024);
            video->rawAudio.resize(video->rawAudio.size()+1024*1024);
        }
        if (fread(video->rawAudio.data()+video->audio_size, header.len, 1, video->fraw) != 1)
        {
            if (feof(video->fraw))
            {
                LOG_INFO("eof occur.");
                return 0;
            }
            LOG_ERROR("Failed to read raw audio frame, err=%d:%s", errno, strerror(errno));
            return -1;
        }
        video->audio_size += header.len;
    }
    {
        AUTOTIMED(("[rawvideo] read body size: "+std::to_string(video->framesize)).c_str(), STAT_RUNTIME);
        if (fread(video->rawBuffer.data(), video->framesize, 1, video->fraw) != 1)
        {
            if (feof(video->fraw))
            {
                LOG_INFO("eof occur.");
                return 0;
            }
            LOG_ERROR("Failed to read frame, err=%d:%s", errno, strerror(errno));
            return -1;
        }
    }

    if (video->pix_fmt == video->out_pix_fmt && video->width == video->disp_width && video->height == video->disp_height)
    {
        *buffer = &video->rawBuffer;
        return 0;
    }

    {
        AUTOTIMED("[rawvideo] scale frame", STAT_RUNTIME);
        if (av_image_fill_arrays(video->frame->data, video->frame->linesize,
            video->rawBuffer.data(), video->pix_fmt, video->width, video->height, 1) < 0)
        {
            LOG_ERROR("av_image_fill_arrays create src image failed");
            return -1;
        }

        int ret = sws_scale(video->swsCtx, video->frame->data,
                video->frame->linesize, 0, video->frame->height, video->displayFrame->data, video->displayFrame->linesize);
        if (ret < 0)
        {
            LOG_ERROR("sws_scale failed");
            return -1;
        }
    }
    *buffer = &video->displayBuffer;
    return 0;
}

static int read_rawaudio_frame(FFReader *video, int max_length)
{
    MsgHead header = { 0 };
    ExtMsgHeadExtAudio extaudioheader = { 0 };

    while(video->aud_fmt) // buffer all raw audio at video->rawAudio until one video frame is encountered
    {
        {
        AUTOTIMED(("[rawaudio] read header, size: "+std::to_string(sizeof(header))).c_str(), STAT_RUNTIME);
        if (fread(&header, sizeof(header), 1, video->fraw) != 1)
        {
            if (feof(video->fraw))
            {
                LOG_INFO("eof occur.");
                break;
            }
            LOG_ERROR("Failed to read rawaudio header, err=%d:%s", errno, strerror(errno));
            return -1;
        }
        }
        if (header.ver != 1 || (header.type != EC_RAWMEDIA_VIDEO &&
            header.type != EC_RAWMEDIA_AUDIO && header.type != EC_RAWMEDIA_EXT_AUDIO))
        {
            LOG_ERROR("Error: bad header in %s, type %d, version %d!", video->decode_video? "rawvideo":"rawaudio", header.type, header.ver);
            return -1;
        }
        if (header.type == EC_RAWMEDIA_VIDEO) // read video and discard
        {
            LOG_ERROR("Warning: read_rawaudio_frame() encounters video, read and discard!");{
            AUTOTIMED(("[rawaudio] read video body size: "+std::to_string(header.len)).c_str(), STAT_RUNTIME);
            video->rawBuffer.resize(header.len);
            if (fread(video->rawBuffer.data(), header.len, 1, video->fraw) != 1)
            {
                if (feof(video->fraw))
                {
                    LOG_INFO("eof occur.");
                    break;
                }
                LOG_ERROR("Failed to read discarding video frame, err=%d:%s", errno, strerror(errno));
                return -1;
            }
            }
            continue;
        }
        extaudioheader.product_id = 0;
        if (header.type == EC_RAWMEDIA_EXT_AUDIO)
        {
            {
            AUTOTIMED(("[rawaudio] read ext audio header, size: 8, body length: " + std::to_string(header.len)).c_str(), STAT_RUNTIME);
            if (fread(&extaudioheader, sizeof(extaudioheader), 1, video->fraw) != 1)
            {
                if (feof(video->fraw))
                {
                    printf("eof occur.\n");
                    break;
                }
                fprintf(stderr, "Failed to read rawaudio ext audio header, err=%d:%s\n", errno, strerror(errno));
                return -1;
            }
            }
        }
        if (extaudioheader.product_id != video->product_id) // switch product, clear buffer
        {
            LOG_ERROR("Switching product from %d to %d", video->product_id, extaudioheader.product_id);
            video->product_id = extaudioheader.product_id;
            video->rawBuffer.clear();
            video->audio_size = 0;
        }
        if (header.len <= 0 || header.len > 1024*1024)
        {
            LOG_ERROR("Error: bad header audio length of %d (max allowed is 1M)!", header.len);
            return -1;
        }
        if(header.len + video->audio_size > video->rawAudio.size())
        {
            LOG_INFO("Warning: increasing rawaudio buffer size to %d", (int)video->rawAudio.size()+1024*1024);
            video->rawAudio.resize(video->rawAudio.size()+1024*1024);
        }
        if (fread(video->rawAudio.data()+video->audio_size, header.len, 1, video->fraw) != 1)
        {
            if (feof(video->fraw))
            {
                LOG_INFO("eof occur.");
                break;
            }
            LOG_ERROR("Failed to read raw audio frame, err=%d:%s", errno, strerror(errno));
            return -1;
        }
        video->audio_size += header.len;
        if (video->audio_size + AUDIO_DELTA >= max_length)
            break;
    }
    return 0;
}

int read_rawvideo_frame_shm(FFReader *video, std::vector<unsigned char> **buffer)
{
    *buffer = NULL;
    MsgHead *header = NULL;
    ExtMsgHeadExtAudio *extaudioheader = NULL;
    static std::vector<unsigned char> rawdata(32*1024*1024);
    int length = 0;
    uint64_t timeout_ms = 0;

    while(true) // buffer all raw audio at video->rawAudio until one video frame is encountered
    {
        AUTOTIMED("[rawvideo_shm] read packet", STAT_RUNTIME);
        struct timeval enTime;
        while((length=sq_get(video->fshm, rawdata.data(), rawdata.size(), &enTime)) == 0) // FIXME: there must be some mechanism to get out of here!!!
        {
            AUTOTIME_SKIP();
            timeout_ms += 10;
            if (timeout_ms > 60000) // 1 minute
            {
                LOG_ERROR("Read rawvideo from shm timeout! path=%s", video->filename.c_str());
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (length < 0)
        {
            LOG_ERROR("Failed to read rawvideo from shm(path=%s), err=%s", video->filename.c_str(), sq_errorstr(video->fshm));
            return -1;
        }
        timeout_ms = 0;
        if (!video->aud_fmt) // pure video frame without header
        {
            if (length != video->framesize) // video
            {
                LOG_ERROR("Error: bad video length in shm(path=%s), expect %d, got %d!", video->filename.c_str(), video->framesize, length);
                return -1;
            }
            unsigned char *data = rawdata.data();
            memcpy(video->rawBuffer.data(), data, video->framesize);
            break;
        }
        if (length < sizeof(MsgHead))
        {
            LOG_ERROR("Error: bad packet length in shm(path=%s), length=%d!", video->filename.c_str(), length);
            return -1;
        }
        header = (MsgHead *)rawdata.data();
        extaudioheader = NULL;
        if (header->ver != 1 || (header->type != EC_RAWMEDIA_VIDEO &&
            header->type != EC_RAWMEDIA_AUDIO && header->type != EC_RAWMEDIA_EXT_AUDIO))
        {
            LOG_ERROR("Error: bad header in shm(path=%s), type %d, version %d!", video->filename.c_str(), header->type, header->ver);
            return -1;
        }
        if (header->type == EC_RAWMEDIA_VIDEO) // VIDEO
        {
            if (header->len != video->framesize) // video
            {
                LOG_ERROR("Error: bad header video length in shm(path=%s), expect %d, got %d!", video->filename.c_str(), video->framesize, header->len);
                return -1;
            }
            if (length < sizeof(MsgHead) + header->len)
            {
                LOG_ERROR("Error: bad packet length for video, expect %d, got %d", sizeof(MsgHead) + header->len, length);
                return -1;
            }
            unsigned char *data = rawdata.data()+sizeof(MsgHead);
            memcpy(video->rawBuffer.data(), data, video->framesize);
            break;
        }
        else // AUDIO
        {
            unsigned char *data = rawdata.data()+sizeof(MsgHead);
            if (header->type == EC_RAWMEDIA_EXT_AUDIO) // ExtAudio
            {
                if (length < sizeof(MsgHead) + sizeof(ExtMsgHeadExtAudio) + header->len)
                {
                    LOG_ERROR("Error: bad packet length for ext audio, expect %d, got %d", sizeof(MsgHead) + sizeof(ExtMsgHeadExtAudio) + header->len, length);
                    return -1;
                }
                extaudioheader = (ExtMsgHeadExtAudio *)(rawdata.data()+sizeof(MsgHead));
                data = rawdata.data()+sizeof(MsgHead)+sizeof(ExtMsgHeadExtAudio);
            }
            if (extaudioheader && extaudioheader->product_id != video->product_id) // switch product, clear buffer
            {
                LOG_INFO("Switching product from %d to %d\n", video->product_id, extaudioheader->product_id);
                video->product_id = extaudioheader->product_id;
                video->audio_size = 0;
            }
            if (header->len <= 0 || header->len > 1024*1024)
            {
                LOG_ERROR("Error: bad header audio length of %d (max allowed is 1M)!", header->len);
                return -1;
            }
            if(header->len + video->audio_size > video->rawAudio.size())
            {
                LOG_ERROR("Warning: increasing rawaudio buffer size to %d", (int)video->rawAudio.size()+1024*1024);
                video->rawAudio.resize(video->rawAudio.size()+1024*1024);
            }
            memcpy(video->rawAudio.data()+video->audio_size, data, header->len);
            video->audio_size += header->len;
        }
    }

    if (video->pix_fmt == video->out_pix_fmt && video->width == video->disp_width && video->height == video->disp_height)
    {
        *buffer = &video->rawBuffer;
        return 0;
    }

    {
        AUTOTIMED("[rawvideo] scale frame", STAT_RUNTIME);
        if (av_image_fill_arrays(video->frame->data, video->frame->linesize,
            video->rawBuffer.data(), video->pix_fmt, video->width, video->height, 1) < 0)
        {
            LOG_ERROR("av_image_fill_arrays create src image failed");
            return -1;
        }

        int ret = sws_scale(video->swsCtx, video->frame->data,
                video->frame->linesize, 0, video->frame->height, video->displayFrame->data, video->displayFrame->linesize);
        if (ret < 0)
        {
            LOG_ERROR("sws_scale failed");
            return -1;
        }
    }
    *buffer = &video->displayBuffer;
    return 0;
}

static int read_rawaudio_frame_shm(FFReader *video, int max_length)
{
    MsgHead *header = NULL;
    ExtMsgHeadExtAudio *extaudioheader = NULL;
    static std::vector<unsigned char> rawdata(32*1024*1024);
    int length = 0;
    uint64_t timeout_ms = 0;

    if (!video->aud_fmt)
    {
        LOG_ERROR("Warning: read_rawaudio_frame_shm() does not support pure video, path=%s", video->filename.c_str());
        return 0;
    }

    while(true) // buffer all raw audio at video->rawAudio until max_length reached
    {
        AUTOTIMED("[rawaudio_shm] read packet", STAT_RUNTIME);
        struct timeval enTime;
        while((length=sq_get(video->fshm, rawdata.data(), rawdata.size(), &enTime)) == 0)
        {
            AUTOTIME_SKIP();
            timeout_ms += 10;
            if (timeout_ms > 60000) // 1 minute
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (length < 0)
        {
            LOG_ERROR("Failed to read rawaudio from shm(path=%s), err=%s", video->filename.c_str(), sq_errorstr(video->fshm));
            return -1;
        }
        if (length < sizeof(MsgHead))
        {
            LOG_ERROR("Error: bad packet length in shm(path=%s), length=%d!", video->filename.c_str(), length);
            return -1;
        }
        timeout_ms = 0;
        header = (MsgHead *)rawdata.data();
        extaudioheader = NULL;
        if (header->ver != 1 || (header->type != EC_RAWMEDIA_VIDEO &&
            header->type != EC_RAWMEDIA_AUDIO && header->type != EC_RAWMEDIA_EXT_AUDIO))
        {
            LOG_ERROR("Error: bad header in shm(path=%s), type %d, version %d!", video->filename.c_str(), header->type, header->ver);
            return -1;
        }
        if (header->type == EC_RAWMEDIA_VIDEO) // VIDEO
        {
            LOG_ERROR("Warning: read_rawaudio_frame_shm() encounters video, discard!");
            continue;
        }
        else // AUDIO
        {
            unsigned char *data = rawdata.data()+sizeof(MsgHead);
            if (header->type == EC_RAWMEDIA_EXT_AUDIO) // ExtAudio
            {
                if (length < sizeof(MsgHead) + sizeof(ExtMsgHeadExtAudio) + header->len)
                {
                    LOG_ERROR("Error: bad packet length for ext audio, expect %d, got %d", sizeof(MsgHead) + sizeof(ExtMsgHeadExtAudio) + header->len, length);
                    return -1;
                }
                extaudioheader = (ExtMsgHeadExtAudio *)(rawdata.data()+sizeof(MsgHead));
                data = rawdata.data()+sizeof(MsgHead)+sizeof(ExtMsgHeadExtAudio);
            }
            if (extaudioheader && extaudioheader->product_id != video->product_id) // switch product, clear buffer
            {
                LOG_INFO("Switching product from %d to %d", video->product_id, extaudioheader->product_id);
                video->product_id = extaudioheader->product_id;
                video->audio_size = 0;
            }
            if (header->len <= 0 || header->len > 1024*1024)
            {
                LOG_ERROR("Error: bad header audio length of %d (max allowed is 1M)!", header->len);
                return -1;
            }
            if(header->len + video->audio_size > video->rawAudio.size())
            {
                LOG_ERROR("Warning: increasing rawaudio buffer size to %d", (int)video->rawAudio.size()+1024*1024);
                video->rawAudio.resize(video->rawAudio.size()+1024*1024);
            }
            memcpy(video->rawAudio.data()+video->audio_size, data, header->len);
            video->audio_size += header->len;
        }
        if (video->audio_size + AUDIO_DELTA >= max_length)
            break;
    }
    return 0;
}


int read_video_frame(FFReader *video, std::vector<unsigned char> **buffer)
{
    if (!video->decode_video)
    {
        *buffer = NULL;
        return 0;
    }

    // read raw data from file and scale
    if (video->fraw || video->fshm)
    {
        int ret;
        if (video->fraw)
            ret = read_rawvideo_frame(video, buffer);
        else
            ret = read_rawvideo_frame_shm(video, buffer);
        if (ret >= 0 && *buffer)
        {
            video->update_time.store(time(NULL));
        }
        return ret;
    }

    // get from buffers
    if (video->buffers.size())
    {
        if(video->displayBuffer.size() != video->buffers[0].size())
        {
            LOG_INFO("Warning: buffer size changed from %u to %u", video->displayBuffer.size(), video->buffers[0].size());
            video->displayBuffer.resize(video->buffers[0].size());
        }
        memcpy(video->displayBuffer.data(), video->buffers[0].data(), video->buffers[0].size());
        video->buffers.erase(video->buffers.begin());
        *buffer = &video->displayBuffer;
        video->update_time.store(time(NULL));
        return 0;
    }

    AVFormatContext* &formatCtx = video->formatCtx;

    int error = 0;

    while(error == 0)
    {
        AVPacket *packet = av_packet_alloc();
        int ret = av_read_frame(formatCtx, packet);
        if (ret < 0) // finished
        {
            LOG_INFO("av_read_frame returns %d (%s)", ret, ret==AVERROR_EOF?"EOF":"UNKNOWN");
            if(ret==AVERROR_EOF) // 继续送进去，读出剩下的帧
            {
                packet->stream_index = video->streamIndex;
            }
            else
            {
                av_packet_unref(packet);
                av_packet_free(&packet);
                error = -1;
                break;
            }
        }
        if (packet->stream_index == video->streamIndex) // video stream
        {
            ret = decode_video_frame(video, packet, buffer);
            av_packet_unref(packet);
            av_packet_free(&packet);
            if (ret >=0 && *buffer)
            {
                time_t currtime = time(NULL);
                if (video->update_time + 1 < currtime && check_is_stream(video->filename.c_str()))
                {
                    LOG_INFO("update_time is too old (%d seconds ago), clear buffers", currtime - video->update_time);
                    video->buffers.clear();
                    video->audio_size = 0;
                    video->audio_adjusted = false; // need to re-adjust audio
                }
                video->update_time.store(currtime);
            }
            if(ret == AVERROR(EAGAIN) && !*buffer) // need more data
                continue;
            if(ret < 0)
                error = (ret==AVERROR_EOF||ret==AVERROR(EAGAIN))? 0 : -1;
            break;
        }
        else if (video->decode_audio && packet->stream_index == video->audioStreamIndex) // audio stream
        {
            int old_audio_size = video->audio_size;
            decode_audio_frame(video, packet);
            av_packet_unref(packet);
            av_packet_free(&packet);
            if (ret >=0)
            {
                time_t currtime = time(NULL);
                if (video->update_time + 1 < currtime && check_is_stream(video->filename.c_str()))
                {
                    LOG_INFO("update_time is too old (%d seconds ago), clear buffers", currtime - video->update_time);
                    video->buffers.clear();
                    memmove(video->rawAudio.data(), video->rawAudio.data() + old_audio_size, video->audio_size - old_audio_size);
                    video->audio_size -= old_audio_size;
                    video->audio_adjusted = false; // need to re-adjust audio
                }
                video->update_time.store(currtime);
            }
            continue;
        }
        // unknown stream
        av_packet_unref(packet);
        av_packet_free(&packet);
    }

    return error;
}

double get_audio_buffer_psttime(FFReader *video)
{
    if (video->audio_pts_time < 0.0)
        return video->audio_pts_time;
    if (video->audio_size <= 0)
        return video->audio_pts_time;

    auto ms = ((double)video->audio_size) / (double)(video->aud_bitrate * 2 * video->aud_channel);
    if (video->audio_pts_time > ms)
        return video->audio_pts_time - ms;
    return 0.0;
}

double get_video_buffer_psttime(FFReader *video)
{
    if (video->video_pts_time < 0.0)
        return video->video_pts_time;
    if (video->buffers.size() <= 0)
        return video->video_pts_time;

    double fps = video->fps < 1.0? 25.0 : video->fps;
    auto ms = (double)(((double)video->buffers.size()) / fps);
    if (video->video_pts_time > ms)
        return video->video_pts_time - ms;
    return 0.0;
}

#define AUDIO_ADJUST_DELTA 1024
#define AUDIO_ADJUST_TIME  0.1 // 100ms

int read_audio_data(FFReader *video, std::vector<unsigned char> &buffer, int max_length)
{
    if (!video->decode_audio)
    {
        buffer.clear();
        return 0;
    }

    AVFormatContext* &formatCtx = video->formatCtx;
    int error = 0;

    // get from buffers
    if(video->audio_size + AUDIO_DELTA >= max_length)
    {
_read_pcm_buffer:
        // adjust audio output according to video pts
        video->audio_adjusted = true; // skip audio adjusting for now
        if (check_is_stream(video->filename.c_str()) && video->decode_video && video->audio_pts_time >= 0.0 && !video->audio_adjusted)
        {
            double v_pts = get_video_buffer_psttime(video);
            double a_pts = get_audio_buffer_psttime(video);
            LOG_INFO("get_audio: v_pts=%f, a_pts=%f, audio_size=%d, video_size=%d", v_pts, a_pts, video->audio_size, video->buffers.size());

            if (a_pts >= 0.0)
            {
                if (v_pts < 0.0)
                {
                    LOG_INFO("Video has not come, hold audio output");
                    buffer.clear();
                    return 0;
                }
                if (a_pts + AUDIO_ADJUST_TIME < v_pts) // audio is too old
                {
                    double delta = v_pts - a_pts;
                    int bytes = video->aud_bitrate * 2 * video->aud_channel * delta;
                    if (bytes > AUDIO_ADJUST_DELTA)
                    {
                        bytes = bytes & 0xffffff00;
                        if (bytes > video->audio_size)
                        {
                            LOG_ERROR("Warning: audio is too old, discard all %d bytes (diff: %d)", video->audio_size, bytes);
                            video->audio_size = 0;
                            buffer.clear();
                            return 0;
                        }
                        if (bytes > 0)
                        {
                            LOG_ERROR("Warning: audio is too old, discard %d bytes (all: %d)", bytes, video->audio_size);
                            memmove(video->rawAudio.data(), video->rawAudio.data() + bytes, video->audio_size - bytes);
                            video->audio_size -= bytes;
                        }
                    }
                }
                else if (v_pts + AUDIO_ADJUST_TIME < a_pts) // audio is too late
                {
                    LOG_ERROR("Warning: audio is too late, hold audio output, v_pts=%f, a_pts=%f", v_pts, a_pts);
                    max_length -= 2;
                    if (max_length > 0)
                    {
                        buffer.resize(max_length);
                        memset(buffer.data(), 0, max_length);
                    }
                    return 0;
                }
                video->audio_adjusted = true;
            }
        }
        if(max_length > video->audio_size || max_length == 0)
            max_length = video->audio_size;
        buffer.resize(max_length);
        memcpy(buffer.data(), video->rawAudio.data(), max_length);
        if(video->audio_size > max_length)
            memmove(video->rawAudio.data(), video->rawAudio.data()+max_length, video->audio_size - max_length);
        video->audio_size -= max_length;
        video->update_time.store(time(NULL));
        return 0;
    }

    // no buffer, but max_length is zeo
    if(max_length == 0)
    {
        buffer.clear();
        return 0;
    }

    if(video->fraw)
    {
        error = read_rawaudio_frame(video, max_length);
        if (error < 0)
            return error;
        video->update_time.store(time(NULL));
        goto _read_pcm_buffer;
    }
    if(video->fshm)
    {
        error = read_rawaudio_frame_shm(video, max_length);
        if (error < 0)
            return error;
        video->update_time.store(time(NULL));
        goto _read_pcm_buffer;
    }

    while(error == 0)
    {
        AVPacket *packet = av_packet_alloc();
        int ret = av_read_frame(formatCtx, packet);
        if (ret < 0) // finished
        {
            LOG_INFO("av_read_frame returns %d (%s)", ret, ret==AVERROR_EOF?"EOF":"UNKNOWN");
            if(ret==AVERROR_EOF) // 继续送进去，读出剩下的帧
            {
                packet->stream_index = video->decode_video? video->streamIndex : video->audioStreamIndex;
            }
            else
            {
                av_packet_unref(packet);
                av_packet_free(&packet);
                error = -1;
                break;
            }
        }
        if (video->decode_video && packet->stream_index == video->streamIndex) // video stream
        {
            ret = decode_video_frame(video, packet, NULL);
            av_packet_unref(packet);
            av_packet_free(&packet);
            if(ret < 0)
            {
                error = (ret==AVERROR_EOF||ret==AVERROR(EAGAIN))? 0 : -1;
                if(ret == AVERROR(EAGAIN) && video->audio_size + AUDIO_DELTA < max_length) // need more data
                    continue;
                if(error==0)
                    goto _read_pcm_buffer;
                break;
            }
            time_t currtime = time(NULL);
            if (video->update_time + 1 < currtime && check_is_stream(video->filename.c_str()))
            {
                LOG_INFO("update_time is too old (%d seconds ago), clear buffers", currtime - video->update_time);
                if (video->buffers.size() > 1) // keep last
                {
                    video->buffers.erase(video->buffers.begin(), video->buffers.begin()+video->buffers.size()-1);
                }
                video->audio_size = 0;
            //    video->audio_adjusted = false; // need to re-adjust audio
            }
            video->update_time.store(currtime);
            if(video->audio_size + AUDIO_DELTA >= max_length)
                goto _read_pcm_buffer;
            continue;
        }
        else if (packet->stream_index == video->audioStreamIndex) // audio stream
        {
            int old_audio_size = video->audio_size;
            ret = decode_audio_frame(video, packet);
            av_packet_unref(packet);
            av_packet_free(&packet);
            if(ret < 0)
            {
                error = (ret==AVERROR_EOF||ret==AVERROR(EAGAIN))? 0 : -1;
                if(ret == AVERROR(EAGAIN) && video->audio_size + AUDIO_DELTA < max_length) // need more data
                    continue;
                if(error==0)
                    goto _read_pcm_buffer;
                break;
            }
            time_t currtime = time(NULL);
            if (video->update_time + 1 < currtime && check_is_stream(video->filename.c_str()))
            {
                LOG_INFO("update_time is too old (%d seconds ago), clear buffers", currtime - video->update_time);
                video->buffers.clear();
                memmove(video->rawAudio.data(), video->rawAudio.data() + old_audio_size, video->audio_size - old_audio_size);
                video->audio_size -= old_audio_size;
            //    video->audio_adjusted = false; // need to re-adjust audio
            }
            video->update_time.store(currtime);
            if(video->audio_size + AUDIO_DELTA >= max_length)
                goto _read_pcm_buffer;
            continue; 
        }
        // unknown stream
        av_packet_unref(packet);
        av_packet_free(&packet);
    }

    return error;
}

int read_video_close(FFReader *&video)
{
    if(video)
    {
        read_video_close_only(video);
        delete video;
        video = nullptr;
    }
    return 0;
}



static int encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt, FILE *outfile)
{
    int ret;

    /* send the frame to the encoder */
    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0)
    {
        fprintf(stderr, "Error sending a frame for encoding\n");
        return -1;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0)
        {
            fprintf(stderr, "Error during encoding\n");
            return -1;
        }

        // printf("Write packet pts=%lu, dts=%lu, stream_idx=%d, duration=%lu, size=%5d\n", (unsigned long)pkt->pts, (unsigned long)pkt->dts, pkt->stream_index, (unsigned long)pkt->duration, pkt->size);
        fwrite(pkt->data, 1, pkt->size, outfile);
    }

    return 0;
}

const char *codec_name = "libx264";
const uint8_t endcode[] = { 0, 0, 1, 0xb7 };

FFVideo *open_video_writer(const char *filename, int width, int height, int bitrate, float fps, const char *preset)
{
    FFVideo *video = new FFVideo;

    /* find the mpeg1video encoder */
    video->codec = avcodec_find_encoder_by_name(codec_name);
    if (!video->codec) {
        delete video;
        fprintf(stderr, "Codec '%s' not found\n", codec_name);
        return NULL;
    }

    video->ctx = avcodec_alloc_context3(video->codec);
    if (!video->ctx) {
        delete video;
        fprintf(stderr, "Could not allocate video codec context\n");
        return NULL;
    }

    video->pkt = av_packet_alloc();
    if (!video->pkt)
    {
        avcodec_free_context(&video->ctx);
        delete video;
        fprintf(stderr, "Could not allocate av packet\n");
        return NULL;
    }

    /* put parameters */
    video->ctx->codec_type = AVMEDIA_TYPE_VIDEO;
    video->ctx->bit_rate = bitrate+512000;
    video->ctx->bit_rate_tolerance = 1000000;
    /* resolution must be a multiple of two */
    video->ctx->width = width;
    video->ctx->height = height;
    /* frames per second */
    video->ctx->time_base = (AVRational){1, (int)fps};
    video->ctx->framerate = (AVRational){(int)fps, 1};
    video->time_base = 1000000 / fps;
    video->ctx->gop_size = 250;
    video->ctx->max_b_frames = 0;
    video->ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    //video->ctx->thread_count = 16;

    if (video->codec->id == AV_CODEC_ID_H264)
    {
        if (preset==NULL || preset[0]==0)
            preset = "slow";
        av_opt_set(video->ctx->priv_data, "preset", preset, 0);
    }

    /* open it */
    int ret = avcodec_open2(video->ctx, video->codec, NULL);
    if (ret < 0) {
        avcodec_free_context(&video->ctx);
        delete video;
        char str[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, str, AV_ERROR_MAX_STRING_SIZE);
        fprintf(stderr, "Could not open codec: %s\n", str);
        return NULL;
    }

    video->f = fopen(filename, "wb");
    if (!video->f) {
        avcodec_free_context(&video->ctx);
        delete video;
        fprintf(stderr, "Could not open %s\n", filename);
        return NULL;
    }

    video->frame = av_frame_alloc();
    if (!video->frame) {
        avcodec_free_context(&video->ctx);
        fclose(video->f);
        delete video;
        fprintf(stderr, "Could not allocate video frame\n");
        return NULL;
    }
    video->frame->format = video->ctx->pix_fmt;
    video->frame->width  = video->ctx->width;
    video->frame->height = video->ctx->height;

    ret = av_frame_get_buffer(video->frame, 0);
    if (ret)
    {
        avcodec_free_context(&video->ctx);
        fclose(video->f);
        av_frame_free(&video->frame);
        delete video;
        fprintf(stderr, "Could not get frame buffer\n");
        return NULL;
    }

    return video;
}

int write_video_frame(FFVideo *video, uint8_t **data, int size, int index)
{
    int ret;
    auto &frame = video->frame;

    ret = av_frame_make_writable(frame);
    if (ret)
    {
        printf("could not make frame writable\n");
        return -1;
    }
    ret = av_image_fill_arrays(frame->data, frame->linesize, data[0], video->ctx->pix_fmt, video->ctx->width, video->ctx->height, 1);
    if (ret < 0)
    {
        printf("av_image_fill_arrays failed.\n");
        return -1;
    }
    frame->pts = index;

    /* encode the image */
    return encode(video->ctx, video->frame, video->pkt, video->f);
}

int write_video_finish(FFVideo *&video)
{
    if (video)
    {
        /* flush the encoder */
        encode(video->ctx, NULL, video->pkt, video->f);

        /* Add sequence end code to have a real MPEG file.
        It makes only sense because this tiny examples writes packets
        directly. This is called "elementary stream" and only works for some
        codecs. To create a valid file, you usually need to write packets
        into a proper file format or protocol; see mux.c.
        */
        if (video->codec->id == AV_CODEC_ID_MPEG1VIDEO || video->codec->id == AV_CODEC_ID_MPEG2VIDEO)
            fwrite(endcode, 1, sizeof(endcode), video->f);
        fclose(video->f);

        avcodec_free_context(&video->ctx);
        av_frame_free(&video->frame);
        av_packet_free(&video->pkt);
        delete video;
        video = nullptr;
    }
    return 0;
}

bool check_video_has_audio_stream(const char *file)
{
    AVFormatContext* formatCtx = NULL;
    int ret;
    int64_t duration_ms;

    if ((ret=avformat_open_input(&formatCtx, file, nullptr, nullptr)) < 0)
    {
        LOG_ERROR("failed to open file %s", file);
        return false;
    }

    if ((ret=avformat_find_stream_info(formatCtx, nullptr)) < 0)
    {
        avformat_close_input(&formatCtx);
        LOG_ERROR("failed to get stream info for %s", file);
        return false;
    }
    ret = av_find_best_stream(formatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ret < 0)
    {
        avformat_close_input(&formatCtx);
        LOG_ERROR("av_find_best_stream not found video stream, path:%s", file);
        return false;
    }

    avformat_close_input(&formatCtx);
    return true;
}

struct RawFrame
{
    int product_id;
    int reopen_time;
    std::vector<unsigned char> video_data;
    std::vector<unsigned char> audio_data;
};

struct RawVFrame
{
    std::vector<unsigned char> video_data;
};

struct RawAFrame
{
    int product_id;
    std::vector<unsigned char> audio_data;
};

class FFVideoDecodeThread;
int cache_video_data(FFVideoDecodeThread *decoder, int frame_num);

class FFVideoDecodeThread
{
public:
    FFVideoDecodeThread() : bExit(false), bStopped(false), bEOF(false), video(NULL), runner(NULL), error_ret(0), cache_mode(AV_TOGETHER), floor_size(0)
    {
    }
    ~FFVideoDecodeThread()
    {
    }
    void RUN()
    {
        const int max_queque_size = 100;
        const int min_queue_size = max_queque_size*8/10;
        while (!bExit)
        {
            if (!bStopped && open_video_reader(video) < 0) // if open failed, try again every 1 second
            {
                videoQueue.Clear();
                vQueue.Clear();
                aQueue.Clear();
                reopen_time.store(time(NULL));
                for(auto i=0; !bExit && i<10; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (bStopped || video==NULL)
            {
                if (bExit) // exit only when queue is empty
                    break;
                if (bStopped)
                {
                    read_video_close_only(video);
                    videoQueue.Clear();
                    vQueue.Clear();
                    aQueue.Clear();
                    reopen_time.store(time(NULL));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (!bEOF)
            {
                int ret = cache_video_data(this, max_queque_size);
                if (ret < 0)
                {
                    if (cache_mode != AV_STREAMING)
                    {
                        // should report it
                        error_ret = ret;
                        break;
                    }
                    // for streaming, we will always retry
                    bEOF.store(true);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (bEOF)
            {
                if (cache_mode == AV_STREAMING)
                {
                    if (check_is_stream(video->filename.c_str()))
                    {
                        // may be camera is down, sleep 1s and try again
                        for(auto i=0; !bExit && i<10; i++)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        LOG_ERROR("Warning: stream %s is down, reconnecting...", video->filename.c_str());
                    }
                    else // normal video, wait until all frames are consumned
                    {
                        while (!bExit && vQueue.Size() > 0)
                        {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                        LOG_DEBUG("%s %s is EOF, rewinding", video->decode_video? "Video" : "Audio", video->filename.c_str());
                    }
                    if(bExit)
                        break;
                    int ret = reopen_video_reader(video);
                    if (ret == 0)
                    {
                        videoQueue.Clear();
                        vQueue.Clear();
                        aQueue.Clear();
                        reopen_time.store(time(NULL));
                        bEOF.store(false); // try again
                    }
                }
                else // for normal video, exit when EOF occurs
                {
                    LOG_INFO("Video decoder thread exit on EOF, file: %s", video->filename.c_str());
                    break;
                }
            }
        }
    }

    void STOP(bool force = false)
    {
        bStopped.store(true);
        if (force)
        {
            videoQueue.Clear();
            vQueue.Clear();
            aQueue.Clear();
            last_frame.audio_data.clear();
            last_frame.video_data.clear();
            last_frame.product_id = 0;
        }
    }
    void INIT(FFReader *writer, CacheMode mode, int floor)
    {
        video = writer;
        bExit.store(false);
        bStopped.store(false);
        bEOF.store(false);
        cache_mode = mode;
        floor_size = floor;
        last_frame.product_id = 0;
        last_frame.reopen_time = 0;
        last_frame.audio_data.clear();
        last_frame.video_data.clear();
        reopen_time = 0;
    }
    void START(FFReader *writer, CacheMode mode, int floor)
    {
        INIT(writer, mode, floor);
        if (runner == NULL)
            runner = new std::thread(&FFVideoDecodeThread::RUN, this);
    }
    void EXIT(bool force = false)
    {
        bExit.store(true);
        if (force)
        {
            videoQueue.Clear();
            vQueue.Clear();
            aQueue.Clear();
            if (runner)
                runner->detach();
        }
        else
        {
            if(runner->joinable())
            {
                runner->join();
            }
        }
        delete runner;
        runner = NULL;
    }
public:
    std::atomic_bool bStopped, bExit, bEOF;
    SafeQueue<struct RawFrame> videoQueue; // for multi-threaded reading
    SafeQueue<struct RawVFrame> vQueue;
    SafeQueue<struct RawAFrame> aQueue;
    FFReader *video;
    std::thread *runner;
    int error_ret;
    CacheMode cache_mode;
    int floor_size;
    struct RawFrame last_frame; // if no data available, display last frame
    std::atomic_int32_t reopen_time;

} ffVideoDecodeThread;

std::map<FFReader*, FFVideoDecodeThread*> decodermap;

// main video use a standalone thread
int start_video_decoder_thread(FFReader *video, CacheMode mode, int floor)
{
    if (video == NULL)
        return 0;

    auto it = decodermap.find(video);
    if (it == decodermap.end())
    {
        FFVideoDecodeThread *decoder = new FFVideoDecodeThread;
        decodermap[video] = decoder;
        decoder->START(video, mode, floor);
    }
    else
    {
        it->second->START(video, mode, floor);
    }
    return 0;
}

int stop_video_decoder_thread(FFReader *video, CacheMode mode, int floor)
{
    if (video == NULL)
        return 0;

    auto it = decodermap.find(video);
    if (it == decodermap.end())
    {
        FFVideoDecodeThread *decoder = new FFVideoDecodeThread;
        decodermap[video] = decoder;
        decoder->INIT(video, mode, floor);
        decoder->STOP(true);
    }
    else
    {
        it->second->STOP(true);
    }
    return 0;
}

int cache_video_data(FFVideoDecodeThread *decoder, int frame_num)
{
    FFReader *video = decoder->video;
    if ((!video->decode_audio && !video->decode_video) || decoder->bEOF)
    {
        // nothing to decode
        return 0;
    }
    double fps = video->fps < 1.0? 25.0 : video->fps;
    double audio_per_frame = ((double)(video->aud_bitrate * 2 * video->aud_channel)) / fps;
    double recv_audio_bytes_f = 0.0;
    int    recv_audio_bytes_i = 0;

    if (decoder->cache_mode == AV_TOGETHER) // MainVideo mode
    {
        while (!decoder->bExit && decoder->videoQueue.Size() < frame_num)
        {
            struct RawFrame data = {0, 0, std::vector<unsigned char>(), std::vector<unsigned char>()};
            std::vector<unsigned char> *buffer = NULL;
            int ret = read_video_frame(video, &buffer);
            if (ret < 0)
            {
                return ret;
            }

            if (buffer)
                data.video_data = *buffer;

            if (video->decode_audio && (video->decode_video==false || data.video_data.size()))
            {
                if (video->decode_video)
                {
                    ret = read_audio_data(video, data.audio_data, 0); // read all
                }
                else
                {
                    int wanted = ((int)(recv_audio_bytes_f + audio_per_frame)) - recv_audio_bytes_i;
                    if (wanted & 1) wanted += 1;
                    ret = read_audio_data(video, data.audio_data, wanted); // read one frame
                    if (ret >= 0)
                    {
                        recv_audio_bytes_f += audio_per_frame;
                        recv_audio_bytes_i += data.audio_data.size();
                    }
                }
                if (ret < 0)
                {
                    return ret;
                }
                data.product_id = video->product_id;
            }

            bool no_audio = data.audio_data.empty();
            if (data.video_data.size() || data.audio_data.size())
                decoder->videoQueue.PushMove(std::move(data));

            // if no video, check audio for EOF
            if ((video->decode_video && buffer==NULL) ||
                 (video->decode_video==false && no_audio))
            {
                LOG_DEBUG("Warning: EOF, decode_video:%d, decode_audio:%d, video_size:%u, audio_size:%u, buffer=%p", (int)video->decode_video, (int)video->decode_audio, data.video_data.size(), data.audio_data.size(), buffer);
                decoder->bEOF.store(true);
                return 0;
            }
        }
    }
    else while(!decoder->bExit) // AV_STREAMING and AV_SEPARATE mode
    {
        int old_updatetime = video->update_time;
        bool has_data = false;
        if (video->decode_video && decoder->vQueue.Size() < frame_num)
        {
            if (video->buffers.size())
            {
                do
                {
                    struct RawVFrame data = {std::vector<unsigned char>()};
                    std::vector<unsigned char> *buffer = NULL;
                    int ret = read_video_frame(video, &buffer);
                    if (ret < 0)
                        return ret;
                    if (!buffer)
                        break;
                    data.video_data = *buffer;
                    decoder->vQueue.PushMove(std::move(data));
                }
                while (video->buffers.size());
            }
            else
            {
                struct RawVFrame data = {std::vector<unsigned char>()};
                std::vector<unsigned char> *buffer = NULL;
                int ret = read_video_frame(video, &buffer);
                if (ret < 0)
                {
                    return ret;
                }

                if (decoder->cache_mode == AV_STREAMING && old_updatetime + 1 < video->update_time)
                {
                    LOG_INFO("Warning: waited too long (%d seconds) for reading a frame, reset buffers", video->update_time - old_updatetime);
                    decoder->vQueue.Clear();
                    decoder->aQueue.Clear();
                    decoder->reopen_time = time(NULL);
                    old_updatetime = video->update_time;
                }

                if (buffer)
                {
                    data.video_data = *buffer;
                    decoder->vQueue.PushMove(std::move(data));
                }
                else
                {
                    decoder->bEOF.store(true);
                    return 0;
                }
            }
            has_data = true;
        }

        if (video->decode_audio && decoder->aQueue.Size() < frame_num)
        {
            int avail_frames = (int)(video->audio_size / audio_per_frame);
            if (avail_frames)
            {
                for (int i=0; i < avail_frames; i++)
                {
                    struct RawAFrame data = {0, std::vector<unsigned char>()};
                    int wanted = ((int)(recv_audio_bytes_f + audio_per_frame)) - recv_audio_bytes_i;
                    if (wanted & 1) wanted += 1;
                    int ret = read_audio_data(video, data.audio_data, wanted); // read one frame
                    if (ret < 0)
                    {
                        return ret;
                    }
                    recv_audio_bytes_f += audio_per_frame;
                    recv_audio_bytes_i += data.audio_data.size();
                    data.product_id = video->product_id;
                    int sz = data.audio_data.size();
                    if (sz)
                        decoder->aQueue.PushMove(std::move(data));
                    if (sz < wanted) // not enough data
                        break;
                }
            }
            else
            {
                struct RawAFrame data = {0, std::vector<unsigned char>()};
                int wanted = ((int)(recv_audio_bytes_f + audio_per_frame)) - recv_audio_bytes_i;
                if (wanted & 1) wanted += 1;
                int ret = read_audio_data(video, data.audio_data, wanted); // read one frame
                if (ret < 0)
                {
                    return ret;
                }
                recv_audio_bytes_f += audio_per_frame;
                recv_audio_bytes_i += data.audio_data.size();
                if (decoder->cache_mode == AV_STREAMING && old_updatetime + 1 < video->update_time)
                {
                    LOG_INFO("Warning: waited too long (%d seconds) for reading audio frame, reset buffers", video->update_time - old_updatetime);
                    decoder->vQueue.Clear();
                    decoder->aQueue.Clear();
                    decoder->reopen_time = time(NULL);
                    recv_audio_bytes_f = 0.0;
                    recv_audio_bytes_i = 0.0;
                }
                data.product_id = video->product_id;
                if (data.audio_data.size())
                    decoder->aQueue.PushMove(std::move(data));
                else if (!video->decode_video)
                {
                    decoder->bEOF.store(true);
                    return 0;
                }
            }
            has_data = true;
        }

        if (!has_data)
            break;
    }
    return 0;
}

// Read mainvideo/rawvideo's data, includeing video frame and corresponding audio data
int read_thread_merge_data(FFReader *video, std::vector<unsigned char> &vdata, std::vector<unsigned char> &adata, int &product_id, int timeout_sec)
{
    auto it = decodermap.find(video);
    if (it == decodermap.end()) // not started?
    {
        LOG_ERROR("Error: video is not in decoder map, make sure it is started by start_video_decoder_thread.");
        return -1;
    }
    FFVideoDecodeThread *decoder = it->second;

    if (decoder->bEOF && decoder->videoQueue.Size()==0)
        return 0;
    
    if (decoder->floor_size > 0)
    {
        int discard = decoder->videoQueue.Floors(decoder->floor_size);
        if (discard > 0)
            LOG_ERROR("Warning: read video thread discards %d frames of data", discard);
    }

    for (int i = 0; timeout_sec == 0 || i < timeout_sec; )
    {
        RawFrame raw;
        if (decoder->videoQueue.PopMove(raw, timeout_sec? 1000 : 0))
        {
            vdata = std::move(raw.video_data);
            adata = std::move(raw.audio_data);
            product_id = raw.product_id;
            return 0;
        }

        if (decoder->error_ret)
            return decoder->error_ret;
        if (decoder->bEOF)
            return 0;
        if (timeout_sec == 0)
        {
            LOG_ERROR("Warning: video data has not arrived, skip.");
            return 1;
        }
        i ++;
        if (i + 1 >= timeout_sec)
        {
            LOG_ERROR("Warning: total timeout (%d seconds) reached for reading from queue", timeout_sec);
            return 1;
        }
        LOG_ERROR("Warning: read from queue timeout (1 sec)");
    }
    return 1;
}

int read_thread_separate_video_frame(FFReader *video, std::vector<unsigned char> &buffer, int timeout_sec)
{
    auto it = decodermap.find(video);
    if (it == decodermap.end()) // not started?
    {
        LOG_ERROR("Error: video is not in decoder map, make sure it is started by start_video_decoder_thread.");
        return -1;
    }
    FFVideoDecodeThread *decoder = it->second;

    if (decoder->bEOF && decoder->vQueue.Size()==0)
        return 0;

    // NOTE: this method does not support discard by floor

    for (int i = 0; timeout_sec == 0 || i < timeout_sec;)
    {
        RawVFrame raw;
        if (decoder->vQueue.PopMove(raw, timeout_sec? 1000 : 0))
        {
            buffer = std::move(raw.video_data);
            return 0;
        }

        if (decoder->error_ret)
            return decoder->error_ret;
        if (decoder->bEOF)
            return 0;
        if (timeout_sec == 0)
        {
            LOG_ERROR("Warning: separate video frame has not arrived, skip.");
            return 1;
        }
        i ++;
        if (i + 1 >= timeout_sec)
        {
            LOG_ERROR("Warning: total timeout (%d seconds) reached for reading separate video frame from queue", timeout_sec);
            return 1;
        }
        LOG_ERROR("Warning: read separate video frame from queue timeout (1 sec)");
    }
    return 1;
}

int read_thread_separate_audio_frame(FFReader *video, std::vector<unsigned char> &buffer, int &product_id, int timeout_sec)
{
    auto it = decodermap.find(video);
    if (it == decodermap.end()) // not started?
    {
        LOG_ERROR("Error: video is not in decoder map, make sure it is started by start_video_decoder_thread.");
        return -1;
    }
    FFVideoDecodeThread *decoder = it->second;

    if (decoder->bEOF && decoder->aQueue.Size()==0)
        return 0;

    for (int i = 0; timeout_sec == 0 || i < timeout_sec;)
    {
        RawAFrame raw;
        if (decoder->aQueue.PopMove(raw, timeout_sec? 1000 : 0))
        {
            product_id = raw.product_id;
            buffer = std::move(raw.audio_data);
            return 0;
        }

        if (decoder->error_ret)
            return decoder->error_ret;
        if (decoder->bEOF)
            return 0;
        if (timeout_sec == 0)
        {
            LOG_ERROR("Warning: separate audio frame has not arrived, skip.");
            return 1;
        }
        i ++;
        if (i + 1 >= timeout_sec)
        {
            LOG_ERROR("Warning: total timeout (%d seconds) reached for reading separate audio frame from queue", timeout_sec);
            return 1;
        }
        LOG_ERROR("Warning: read separate audio frame from queue timeout (1 sec)");
    }
    return 1;
}

int read_thread_stream_video_frame(FFReader *video, std::vector<unsigned char> **buffer)
{
    auto it = decodermap.find(video);
    if (it == decodermap.end()) // not started?
    {
        LOG_ERROR("Error: video is not in decoder map, make sure it is started by start_video_decoder_thread.");
        return -1;
    }
    FFVideoDecodeThread *decoder = it->second;

    LOG_DEBUG("vQueue size %d, aQueue size %d, floor %d, video->vbufsiz %d, video->abufsize %d", decoder->vQueue.Size(), decoder->aQueue.Size(), decoder->floor_size, video->buffers.size(), video->audio_size);
    if (decoder->floor_size > 0 && decoder->vQueue.Size() >= decoder->floor_size)
    {
        if (video->decode_audio)
        {
            double fps = video->fps < 1.0? 25.0 : video->fps;
            int may_discard = decoder->vQueue.Size() - (decoder->floor_size / 2);
            double bytes_per_frame = (((double)(video->aud_bitrate * 2 * video->aud_channel)) / fps); // 640
            int may_discard_audio = (int)(bytes_per_frame * may_discard);

            std::vector<unsigned char> audio;
            int product_id;
            int ret = read_thread_stream_audio_frame(video, audio, product_id, may_discard_audio);
            if (ret >= 0)
            {
                double discarded_audio = (double)audio.size();
                auto discard = discarded_audio / bytes_per_frame + 0.5;
                decoder->vQueue.Discard((int)discard);
                LOG_ERROR("Warning: read stream video discards %d frames of video, along with %d bytes of audio", (int)discard, audio.size());
            }
        }
        else
        {
            int discard = decoder->vQueue.Floors(decoder->floor_size / 2);
            if (discard > 0)
            {
                LOG_ERROR("Warning: we are reading too many video frames at a time, discard %d frames", discard);
            }
        }
    }

    RawVFrame raw;
    while (decoder->vQueue.PopMove(raw, 0))
    {
        if (raw.video_data.empty()) // no video in frame
            continue;

        decoder->last_frame.video_data = std::move(raw.video_data);
        *buffer = &decoder->last_frame.video_data;
        return 0;
    }

    if (decoder->error_ret)
        return decoder->error_ret;
    if (decoder->last_frame.video_data.empty())
    {
        *buffer = NULL;
        return 0;
    }
    *buffer = &decoder->last_frame.video_data;
    return 0;
}

int read_thread_stream_audio_frame(FFReader *video, std::vector<unsigned char> &buffer, int &product_id, int max_length)
{
    auto it = decodermap.find(video);
    if (it == decodermap.end()) // not started?
    {
        LOG_ERROR("Error: video is not in decoder map, make sure it is started by start_video_decoder_thread.");
        return -1;
    }
    FFVideoDecodeThread *decoder = it->second;

    if (!decoder->video->decode_audio) // no audio
    {
        buffer.clear();
        product_id = 0;
        return 0;
    }

    if (decoder->last_frame.reopen_time != decoder->reopen_time)
    {
        decoder->last_frame.audio_data.clear();
        decoder->last_frame.reopen_time = decoder->reopen_time;
        fprintf(stdout, "Warning: video is reopenned\n");
    }
#if 0
    if (decoder->floor_size > 0 && decoder->aQueue.Size() >= decoder->floor_size)
    {
        int discard = decoder->aQueue.Floors(decoder->floor_size);
        if (discard > 0)
        {
            double fps = video->fps < 1.0? 25.0 : video->fps;
            double bytes_per_frame = (((double)(video->aud_bitrate * 2 * video->aud_channel)) / fps);
            int discard_audio = (int)(bytes_per_frame * discard);
            LOG_ERROR("Warning: read stream audio discards %d frames (%d bytes)", discard, discard_audio);
        }
    }
#endif
    while (max_length > decoder->last_frame.audio_data.size() + AUDIO_DELTA)
    {
        RawAFrame raw;
        if (decoder->aQueue.PopMove(raw, 0)) // read video and discard
        {
            decoder->last_frame.audio_data.insert(decoder->last_frame.audio_data.end(), raw.audio_data.begin(), raw.audio_data.end());
        }
        else if (max_length > decoder->last_frame.audio_data.size() + AUDIO_DELTA)
        {
            LOG_ERROR("Warning: not enough audio data, wanted %d, has %d, filename: %s", max_length, decoder->last_frame.audio_data.size(), video->filename.c_str());
            max_length = decoder->last_frame.audio_data.size();
            break;
        }
    }
    if (max_length > decoder->last_frame.audio_data.size())
        max_length = decoder->last_frame.audio_data.size();
    buffer.clear();
    buffer.insert(buffer.end(), decoder->last_frame.audio_data.begin(), decoder->last_frame.audio_data.begin()+max_length);
    if (max_length < decoder->last_frame.audio_data.size())
        decoder->last_frame.audio_data.erase(decoder->last_frame.audio_data.begin(), decoder->last_frame.audio_data.begin()+max_length);
    else
        decoder->last_frame.audio_data.clear();
    product_id = decoder->last_frame.product_id;

    return 0;
}



int close_video_thread(FFReader *video, bool force)
{
    auto it = decodermap.find(video);
    if (it == decodermap.end()) // not started?
    {
        LOG_ERROR("Error: video is not in decoder map, make sure it is started by start_video_decoder_thread.");
        return -1;
    }
    FFVideoDecodeThread *decoder = it->second;
    decoder->EXIT(force);
    // if force, will return quickly without freeing resources
    if (!force)
    {
        read_video_close(decoder->video);
        delete decoder;
        decodermap.erase(it);
    }
    return 0;
}
