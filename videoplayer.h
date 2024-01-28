#ifndef __VIDEO_PLAYER_HEADER__
#define __VIDEO_PLAYER_HEADER__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "safequeue.h"
#include "3rd/shmqueue/shm_queue.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}


struct FFReader
{
    std::string filename;
    AVFormatContext *formatCtx;
    // for video
    std::vector<unsigned char> displayBuffer;
    AVFrame *frame;
    AVFrame *displayFrame;
    AVCodecContext *avCodecCtx;
    SwsContext *swsCtx;
    const char *scale_prefer;
    // for audio
    AVFrame *audioFrame;
    AVCodecContext *avAudioCodecCtx;
    AVCodecParameters *avAudioCodecParam;
    SwrContext *m_swrCtx;

    int width, height; // original size
    int disp_width, disp_height; // display size
    int colorSpace, colorRange;
    bool decode_video;
    bool decode_audio;
    double fps;
    double rotation; // rotation in video stream's side data
    int64_t bitrate;
    int streamIndex;
    int audioStreamIndex;
    int buffersize;
    int64_t totaltime;
    double video_timebase;
    double audio_timebase;
    double video_pts_time;
    double audio_pts_time;
    std::atomic_bool audio_adjusted; // only adjust for first frame
    std::vector<std::vector<unsigned char>> buffers;

    AVPixelFormat pix_fmt, out_pix_fmt;
    int framesize;
    char *aud_fmt;
    AVSampleFormat aud_samplefmt;
    int aud_channel;
    int64_t aud_bitrate;
    int audio_size;
    int product_id; // latest product id in rawAudio
    std::atomic_int32_t update_time; // timestamp for last packet arrival
    std::vector<unsigned char> rawBuffer;
    std::vector<unsigned char> rawAudio;
    FILE *fraw;
    struct shm_queue *fshm;
};

// Open a video file for reading, and scale to widthxheight if non zero
FFReader *open_video_reader(const char *filename, bool decode_video, bool decode_audio,
                            int width, int height, const char *scale_prefer, AVPixelFormat out_pixfmt,
                            AVSampleFormat out_audfmt, int out_channel, int64_t out_samplerate);
// Create a video instance, you should call open_video_reader afterwards
FFReader *new_video_reader(const char *filename, bool decode_video, bool decode_audio,
                            int width, int height, const char *scale_prefer, AVPixelFormat out_pixfmt,
                            AVSampleFormat out_audfmt, int out_channel, int64_t out_samplerate);
// Open video reader created by new_video_reader
int open_video_reader(FFReader *video);

// Read a frame in BGRA format
int read_video_frame(FFReader *video, std::vector<unsigned char> **buffer, int &raw_image_index);
// Read audio data of up to max_length size, read all in buffer if max_length == 0
int read_audio_data(FFReader *video, std::vector<unsigned char> &buffer, int max_length);

FFReader *open_raw_video(const char *file, int disp_width, int disp_height, const char *scale_prefer,
                         const char *pix_fmt, const char *out_pix_fmt, int width, int height, float fps, int64_t bitrate,
                         const char *aud_fmt, int aud_channel, int64_t aud_bitrate);

int read_video_close(FFReader *&video);

int64_t get_video_duration(const char *file);
bool check_video_has_audio_stream(const char *file);

struct FFVideo
{
    const AVCodec *codec;
    // OutputStream video_st;
    FILE *f;
    AVFrame *frame;
    AVPacket *pkt;
    AVCodecContext *ctx;
    double time_base; //in us
};

// Open a video file for writting
FFVideo *open_video_writer(const char *filename, int width, int height, int bitrate, float fps, const char *preset);
int write_video_frame(FFVideo *video, uint8_t **data, int size, int index);
int write_video_finish(FFVideo *&video);

struct pcm_info {
    int length; // num of pcm[]
    short *pcm;
    int volum; // 0 - 100
};

// must be pcm_s16le format
void mverge_audio(std::vector<pcm_info> &pcms, int pcm_length, std::vector<unsigned char> &out);

//
// Multithreading support for video decoding
//

enum CacheMode {
    AV_TOGETHER,  // for mainvideo, read video frame by frame, read all available audio into the current frame
    AV_SEPARATE,  // for normal video, read video and audio separately, both frame by frame
    AV_STREAMING, // for network streaming, read available video frame with equal length audio
};
int start_video_decoder_thread(FFReader *video, CacheMode mode, int floor);
int stop_video_decoder_thread(FFReader *video, CacheMode mode, int floor);

// Read video data, includeing video frame and corresponding audio data, should be started with AV_TOGETHER mode
int read_thread_merge_data(FFReader *video, std::vector<unsigned char> &vdata, std::vector<unsigned char> &adata, int &raw_image_index, int &product_id, int timeout_sec);

// Read video video/audio separately, should be started with AV_SEPARATE mode
// Returns:
//    < 0 : failure
//   == 0 : success
//   == 1 : timeout
int read_thread_separate_video_frame(FFReader *video, std::vector<unsigned char> &buffer, int &raw_image_index, int timeout_sec);
int read_thread_separate_audio_frame(FFReader *video, std::vector<unsigned char> &buffer, int &product_id, int timeout_sec);

// Read thread streaming data, should be started with AV_STREAMING mode
int read_thread_stream_video_frame(FFReader *video, std::vector<unsigned char> **buffer, int &raw_image_index);
// Read audio data of up to max_length size, read all in buffer if max_length == 0
int read_thread_stream_audio_frame(FFReader *video, std::vector<unsigned char> &buffer, int &product_id, int max_length);

int close_video_thread(FFReader *video, bool force = false);



#endif
