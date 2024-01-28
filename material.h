#pragma once
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>
#include "videoplayer.h"
#include "videowriter.h"


struct materialcontext
{
    int w,h;
    double fps;
    int findex; // current frame index in frames for gif
    double cts; // current read time stamp from 0.0 milisecond
    double tstep; // time stamp between two consecutive frames, that is 1000/fps
    FFReader *reader;
    // frame type: 
    //   bgrm  - bgr with mask
    //   ibgra - inverted image of brga
    enum ColorType { FT_None, FT_BGR, FT_BGRA, FT_BGRM, FT_IBGRA } ftype;
    std::vector<cv::Mat> frames;
    std::vector<unsigned char> audio; // raw pcm s16le data
    std::vector<double> fps_times; // each frame's display time in second, e.g. 0.01s
    int bufindex; // current index in frames
    int audindex; // current index in audio
    int time_opacity, time_rotation;
    double clock_x_ratio, clock_y_ratio;

    unsigned int glTexture; // opengl texture for rendering
};

struct material
{
    int layer;
    enum MaterialType { MT_None, MT_MainVideo, MT_MainAudio, MT_Video, MT_Audio, MT_Gif, MT_Image, MT_Text, MT_Time, MT_Clock } type;
    int product_id; // >= 1, 0 for all products
    int material_id; // used to delete/modify
    char text[1024];
    char path[1024];
    cv::Rect rect;
    cv::Rect ori_video_rect; // video's original rect, when rotation is non zero, rect will be enlarged to fit the rotated image
    int volume; // audio volume, 0-100
    int rotation; // clockwise rotation degree, -360 - 360
    int opacity; // opacity percentage, 0 - 100

    // text attributes
    char font[1024];
    int fontsize, olsize; // ol for outline
    unsigned char color[4], olcolor[4];

    materialcontext ctx;
};

struct rawvideoinfo
{
    const char *pix_fmt;
    int w, h;
    int fps;
    int64_t bitrate;
};

struct rawaudioinfo
{
    const char *aud_fmt;
    int channel;
    int64_t samplerate;
};

struct streamoutinfo
{
    // stream info
    char *rtmpout;

    // video/audio info
    rawvideoinfo videoinfo; // substream's pix_fmt should be same as main output's pix_fmt
    rawaudioinfo audioinfo; // substream's audioinfo should be same as main output's audioinfo
    bool disable_audio;     // no audio output

    FILE *writer_pipe; // fifo fd to ffmpeg popen
    FFVideoEncodeThread *video_thread;
    FFAudioEncodeThread *audio_thread;
    char audio_fifo[1024]; // audio fifo to ffmpeg, for rtmpout
    char streamspec[10240];

    int start_frameno;
    int cur_frameno;

    streamoutinfo() : rtmpout(NULL), videoinfo{NULL}, audioinfo{NULL}, \
                      disable_audio(0), writer_pipe(NULL), video_thread(NULL), audio_thread(NULL), \
                      audio_fifo{0}, streamspec{0}, start_frameno(0), cur_frameno(0)
    {
    }
};

// --watermark=text:font:size:#rgbcolor:rotation:opacity:rows:cols:top:left:width:height:#olcolor
struct watermark
{
    char *text;
    char *font;
    int size;
    unsigned char bgr[3]; // font color bgr
    unsigned char olbgr[4]; // outline color bgr
    int rotation;
    int opacity;
    int rows; // repeat rows, <=1 for no repeat
    int cols; // repeat cols
    int x, y, w, h;
};

material::MaterialType string2type(const std::string &s);
void merge_path(char *out, int out_size, const char *dir, const char *path);
void merge_pathlist(char *out, int out_size, const char *dir, const char *pathlist);
std::vector<std::string> splitString(const std::string &str, char sep);
std::vector<char *> splitCString(char *str, char sep);
bool file_exists(const std::string &file);
void merge_path(char *out, int out_size, const char *dir, const char *path);
int ensure_dir_exists(const char *path);
int parse_color(const char *colorstr, std::vector<unsigned char> &bgr);

bool check_is_stream(const char *path);
bool check_is_stream(material &m);
bool check_is_audio(material &m);
bool check_has_stream(std::vector<material> &mlist);
int parse_material(const std::string &s, material &mm, const char *data_dir, double x_ratio, double y_ratio);
int parse_materials(int argc, char **argv, material &mainvideo, material &mainaudio, std::vector<material> &mlist, const char *data_dir, double x_ratio, double y_ratio);

int open_materials(std::vector<material> &mlist, double x_ratio, double y_ratio, rawaudioinfo *audioinfo, material *mainaudio, int stream_buffer_size, int out_fps, bool disable_opengl, int product_id);
// open materials in new thread
int submit_open_materials(std::vector<material> &new_mlist, double x_ratio, double y_ratio, rawaudioinfo &rawaudio, material &mainaudio, int stream_buffer_size, int fps, bool disable_opengl, int product_id);
std::vector<material> *check_open_materials();
void close_material(material &m);
int read_next_audio(material &m, std::vector<unsigned char> &buf, int max_size);
cv::Mat *read_next_frame(material &m, double ts, bool disable_opengl);// read a proper frame based on the current timestamp

std::string format_ffmepg_encode_cmdline(const std::string ffmpeg, const char *output_video,
                    int output_width, int output_height, int fps, int bitrate, bool use_nvenc,
                    const char *encode_preset, const char *output_fmt, const char *rawaudio_file,
                    int rawaudio_channel, int64_t rawaudio_samplerate, const char *rtmpurl, bool quiet);

int start_streamout_thread(streamoutinfo *stream, const std::string &ffmpeg);

int stop_streamout_thread(streamoutinfo *stream, bool force);
int close_streamout_thread(streamoutinfo *stream, bool force);

void replace_all(std::string &s, const std::string &from, const std::string &to);

