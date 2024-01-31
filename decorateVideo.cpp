#include <unistd.h>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <thread>
#include <signal.h>
#include <execinfo.h>
#include "3rd/gifdecoder/ffgif.h"
#include "3rd/gifdecoder/gifdec.h"
#include "videoplayer.h"
#include "version.h"
#include "gl_render.h"
#include "AutoTime.h"
#include "matops.h"
#include "decorateVideo.h"
#include "safequeue.h"
#include "event.h"
#include "videowriter.h"
#include "3rd/log/LOGHelp.h"
#include "material.h"
#include "stream_cmd.h"
#include "filter/watermark.h"

#undef	__MODULE__
#define __MODULE__ "Main"

using namespace std;
using namespace cv;

#define MAX_LOG_SIZE (10*1024*1024)

int enable_debug = 0;


static std::string get_ffmpeg_path()
{
    std::string ffmpeg = "/var/local/decorate_video/ffmpeg";
    if(file_exists(ffmpeg.c_str()))
    {
        return ffmpeg;
    }

    if(system("ffmpeg -version|grep 'version'") == 0)
    {
        return "ffmpeg";
    }

    std::cerr << "ffmpeg not found, please install it first." << std::endl;
    return "";
}

static streamoutinfo substream_out;
static string out_audio_fifoname;
static pthread_t main_thread_id = 0;

void DumpTraceback(int signal) {
    const int size = 200;
    void *buffer[size];
    char **strings;

    int nptrs = backtrace(buffer, size);
    printf("backtrace() returned %d address\n", nptrs);

    strings = backtrace_symbols(buffer, nptrs);
    if (strings) {
        for (int i = 0; i < nptrs; ++i) {
            printf("[#%d]%s\n", i, strings[i]);
        }
        free(strings);
    }
}

static volatile std::atomic_bool exit_signal_handled(false);

extern "C" void sig_handler(int signo)
{
    if (main_thread_id != pthread_self())
    {
        fprintf(stderr, "Receive signal %d in non-main thread.\n", signo);
        if (signo == SIGPIPE && !exit_signal_handled)
        {
            pthread_kill(main_thread_id, SIGPIPE);
        }
        if (signo != SIGABRT)
        {
            pthread_exit(NULL);
        }
        return;
    }

    if (signo == SIGUSR1 || signo == SIGUSR2 || exit_signal_handled) // ignore
    {
        if (signo == SIGABRT || signo == SIGKILL || signo == SIGSEGV)
            _exit(1);
        fprintf(stderr, "Received signal(%d), ignored\n", signo);
        return;
    }
    exit_signal_handled = true;

    fprintf(stderr, "Received term signal(%d), exiting.\n", signo);
    if (signo == SIGABRT || signo == SIGSEGV)
    {
        DumpTraceback(signo);
    }
    if (out_audio_fifoname.length())
    {
        ffVideoEncodeThread.EXIT(true);
        ffAudioEncodeThread.EXIT(true);
        remove(out_audio_fifoname.c_str());
    }
    // close_streamout_thread(&substream_out, true);
    if (substream_out.audio_fifo[0])
        remove(substream_out.audio_fifo);

    //signal(signo, SIG_IGN);
    if (signo != SIGABRT && signo != SIGKILL && signo != SIGSEGV)
        _exit(0);
}

int main(int argc, char **argv) {

    main_thread_id = pthread_self();
    const char *data_dir = "";
    const char *log_file = "";
    for (int i=0; i<argc; i++)
    {
        const char *opt = "--log_file=";
        int optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            log_file = argv[i]+optlen;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--data_dir=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            data_dir = argv[i]+optlen;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
    }
    // redirect stderr/stdout
    if (log_file && log_file[0])
    {
        char path[1024];
        merge_path(path, sizeof(path), data_dir, log_file);
        if (ensure_dir_exists(path) < 0)
            return -1;

        string log = path+string("_log.txt"), err = path+string("_err.txt");
        FILE *l, *e;
        l = fopen(log.c_str(), "a");
        e = fopen(err.c_str(), "a");
        if (l==NULL || e==NULL)
        {
            if (l) fclose(l);
            if (e) fclose(e);
            std::cerr << "Failed to open log file " << (l==NULL? log : err) << std::endl;
            return -1;
        }

        // default to append log files, but if file is too large, use overwrite
        const char *lflag = "a", *eflag = "a";
        if (ftell(l) > MAX_LOG_SIZE) lflag = "w";
        if (ftell(e) > MAX_LOG_SIZE) eflag = "w";
        fclose(l);
        fclose(e);
        stdout = freopen(log.c_str(), lflag, stdout);
        stderr = freopen(err.c_str(), eflag, stderr);

        // write time info to log files
        time_t t = time(NULL);
        struct tm *lt = localtime(&t);
        char ch[100];
        snprintf(ch, sizeof(ch), "%02d-%02d-%02d %02d:%02d:%02d", lt->tm_year+1900, lt->tm_mon+1,
                    lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
        const char *p = strchr(argv[1], ':');
        string ov = p? string(argv[1], p-argv[1]) : string(argv[1]);
        std::cerr << std::endl << "========= " << ch << ": error log for " << ov << " =========" << std::endl;
        std::cout << std::endl << "========= " << ch << ": info log for " << ov << " =========" << std::endl;
    }

    std::cout << "General video decorate tool, version " << SDK_VERSION << std::endl;

    if (argc < 3) {
        std::cout << std::endl;
        std::cout << "Bad argument, usage:" << std::endl;
        std::cout << argv[0] << " <options> <output_video_param> <material_1> <material_2> ..." << std::endl;
        std::cout << std::endl;
        std::cout << "Available options: " << std::endl;
        std::cout << "  --bg_color=#ffaabb                    # set background color to #ffaabb" << std::endl;
        std::cout << "  --enable_chromakeying                 # enable chroma keying (removing green background) on mainvideo, for test purposes only," << std::endl;
        std::cout << "                                        # for product use, please use professional software such as Premiere Pro and pruduce left-right or webm video" << std::endl;
        std::cout << "  --disable_opengl                      # disable opengl rendering, run in pure CPU mode" << std::endl;
        std::cout << "  --enable_window                       # display the preview window while processing" << std::endl;
        std::cout << "  --enable_debug                        # output debug message while processing" << std::endl;
        std::cout << "  --enable_ff_nv_enc                    # enable hw encode for nvidia driver, only for mp4 file output" << std::endl;
        std::cout << "  --data_dir=<data_dir>                 # set root data dir for all path parameters" << std::endl;
        std::cout << "  --log_file=<path>                     # set log/error file to <path>_log.txt and <path>_err.txt, default to stdout/stderr" << std::endl;
        std::cout << "  --scale_engine=ffmpeg|opengl|opencv   # set scale lib, default is opengl" << std::endl;
        std::cout << "  --scale_prefer=quality|speed|both     # set scale preference, default is both" << std::endl;
        std::cout << "  --video_size=<width>x<height>         # rescale the decorated video to size width x height" << std::endl;
        std::cout << "                                        # note that all materials' size parameters are with reference to width/height specified in output_video parameters" << std::endl;
        std::cout << "  --encode_preset=slow|medium|fast      # set encode preset" << std::endl;
        std::cout << "  --alpha_video=left|right|top|bottom   # auto detect alpha video in mainvideo" << std::endl;
        std::cout << "  --alpha_egine=opengl|opencv           # set engine used to process alpha video" << std::endl;
        std::cout << "  --read_timeout=<sec>                  # timeout in seconds for reading mainvideo" << std::endl;
        std::cout << "  --rawvideo=fmt:w:h:fps:bitrate        # read mainvideo as rawdata, specify video format" << std::endl;
        std::cout << "                                        # fmt must be one of \"bgr\", \"bgra\", \"rgb\"" << std::endl;
        std::cout << "  --rawaudio=fmt:samplerate:channelnum  # read mainvideo as rawvideo + rawaudio, fmt must be \"pcm_s16le\"" << std::endl;
        std::cout << "                                        # mainvideo/mainaudio format: <dwType><dwLength><ExtHeader><acValue>" << std::endl;
        std::cout << "                                        #    dwType:   4 bytes, host order, 1 - rawvideo; 2 - rawaudio; 3 - audio with ExtHeader(uint32 product_id, host order)" << std::endl;
        std::cout << "                                        #    dwLength: 4 bytes, host order, packet length or length of acValue" << std::endl;
        std::cout << "                                        #    acValue:  dwLength bytes, video or audio packet data" << std::endl;
        std::cout << "  --stream_out=protocol://proto_spec    # protocol can be rtmp, this option must be use with '-' output filename" << std::endl;
        std::cout << "                                        # for rtmp: rtmp://server/url/streamname" << std::endl;
        std::cout << "  --substream_out=protocol://proto_spec:<width>:<height>:<framerate>:<bitrate>:<disable_audio(0|1)> # support a sub stream output, currently only support rtmp" << std::endl;
        std::cout << "  --stream_cmd_fifo=fifo_file           # streaming controlling fifo in text line, format:" << std::endl;
        std::cout << "                                        #     <operation><space><arguments_separated_by_comma>\\n" << std::endl;
        std::cout << "                                        # Available operations:" << std::endl;
        std::cout << "                                        #     ADD <product_id>:<material_id>:<material_spec>" << std::endl;
        std::cout << "                                        #     DEL <product_id>:<material_id>" << std::endl;
        std::cout << "                                        #     MOD <product_id>:<material_id>:<layer>:<top>:<left>:<width>:<height>" << std::endl;
        std::cout << "                                        #     SUBOUT rtmp://<rtmp_spec>:<width>:<height>:<fps>:<bitrate>:<disable_audio(0|1)> # add substream output, only support rtmp" << std::endl;
        std::cout << "                                        #     STOPSUB # stops current substream output" << std::endl;
        std::cout << "                                        #     SWPROD <product_id> # switch to new product" << std::endl;
        std::cout << "                                        # Examples:" << std:: endl;
        std::cout << "                                        #     eg.1: ADD 0:13:text:2:TestVideo:0:0:400:200:20:0:0:#0ff:10:#ff0" << std::endl;
        std::cout << "                                        #     eg.2: DEL 0:13" << std::endl;
        std::cout << "                                        #     eg.3: MOD 0:13:3:100:100:400:200" << std::endl;
        std::cout << "                                        #     eg.4: SUBOUT rtmp://test_server.com/testdomain/teststream:1080:1920:25:2000000:0" << std::endl;
        std::cout << "  --stream_cmd_txtfile=file             # streaming controlling file, same as stream_cmd_fifo except the file is a normal text file" <<std::endl;
        std::cout << "  --stream_buffer_size=size             # stream buffer size, keep only most recent <size> frames in buffer" << std::endl;
        std::cout << "  --notify_fifo_event=fifo_file         # notify event to caller by named fifo, format:" << std::endl;
        std::cout << "                                        #     {\"code\":\"123\", \"timestamp\":\"112233\", \"message\":\"\"}" << std::endl;
        std::cout << "                                        # Available codes:" << std::endl;
        for (auto &c : event_codes)
        {
        std::cout << "                                        #     " << c.e << ": " << c.desc << std::endl;
        }
        std::cout << "  --watermark=text:font:size:#rgbcolor:rotation:opacity:rows:cols:top:left:width:height:#olcolor" << std::endl;
        std::cout << "                                        # set water mark, parameters:" << std::endl;
        std::cout << "                                        #       text: text as water mark" << std::endl;
        std::cout << "                                        #       font: system font name or full file path, see fc-list output" << std::endl;
        std::cout << "                                        #       size: font size, recommended is 18" << std::endl;
        std::cout << "                                        #      color: font color in #RRGGBB format" << std::endl;
        std::cout << "                                        #   rotation: rotation degree, from -360 to 360" << std::endl;
        std::cout << "                                        #    opacity: transparency pencentage, from 1 to 100, 0 is equal to 100, fully opaque" << std::endl;
        std::cout << "                                        #       rows: repeated row number in non-rotated state, 0 for auto calculation" << std::endl;
        std::cout << "                                        #       cols: repeated collum number in non-rotated state, 0 for auto calculation" << std::endl;
        std::cout << "                                        #  top|left|width|height: text position for non-repeated water mark" << std::endl;
        std::cout << "                                        #    olcolor: outline color of text in #RRGGBB format" << std::endl;
        std::cout << "  --subtitle=file:top:left:width:height:style  # set subtitle file, position and style (ffmpeg subtitles:force_style)" << std::endl;
        std::cout << "                                        # see https://www.myzhenai.com.cn/post/4153.html for force_style" << std::endl;
        std::cout << "  --use_ffmpeg=file                     # use ffmpeg program file, default is /var/local/decorate_video/ffmpeg" << std::endl;
        std::cout << "  --disable_ffmpeg_stat                 # disable ffmpeg statistic output" << std::endl;
        std::cout << "  --blind_watermark=<type(text|image)>:<text|image_path>:<interval>" << std::endl;
        std::cout << "                                        # set blind water mark, parameters:" << std::endl;
        std::cout << "                                        #      type: text or image as water mark" << std::endl;
        std::cout << "                                        #  interval: N - add 1 frame watermark every N seconds" << std::endl;
        std::cout << "                                        #            0 - no watermark" << std::endl;
        std::cout << "                                        #           -1 - add watermark on every frame" << std::endl;
        std::cout << std::endl;
        std::cout << "Output video param:" << std::endl;
        std::cout << "  <filename>:<width>:<height>:<framerate>:<bitrate>:<fmt>" << std::endl;
        std::cout << "    - filename: local file name or - for streaming output, see --stream_out option" << std::endl;
        std::cout << "    - width/height: the output video's width/height, must be specified." << std::endl;
        std::cout << "    - framerate: optional, frames per second, default to main video's fps" << std::endl;
        std::cout << "    - bitrate: optional, bits per second, default to main video's bitrate" << std::endl;
        std::cout << "    - fmt: one of {mp4, mp4alpha, mov, webm, raw24, raw32}, default is mp4" << std::endl;
        std::cout << "         - mp4: libx264(yuv420p) + aac encoding" << std::endl;
        std::cout << "         - mp4alpha: mp4 with right-side(1080x1920) or bottom-side(1920x1080) alpha video" << std::endl;
        std::cout << "         - mov: qtrle(argb with alpha) + aac encoding, *** caution: very very large!" << std::endl;
        std::cout << "         - webm: libvpx-vp9(yuva420p with alpha) + libopus encoding, *** caution: very very slow!" << std::endl;
        std::cout << "         - raw24: bgr24 + pcm_s16le in {type + length + data} format" << std::endl;
        std::cout << "         - raw32: bgra + pcm_s16le in {type + length + data} format" << std::endl;
        std::cout << std::endl;
        std::cout << "Material format for media:" << std::endl;
        std::cout << "  <type>:<layer>:<path>:<top>:<left>:<width>:<height>:<volume>:<rotation>:<opacity>" << std::endl;
        std::cout << "    - type: one of mainvideo, mainaudio, image, gif, video, audio. " << std::endl;
        std::cout << "    - layer: layer starting at 1, used for sorting the materials. " << std::endl;
        std::cout << "    - path: file path or rtmp://domain/appid/streamid or shm://shm_id" << std::endl;
        std::cout << "    - top/left/width/height: position and size relative to the output video " << std::endl;
        std::cout << "    - volume: optional, for video/audio only, the sound volume, range 0-100" << std::endl;
        std::cout << "    - rotation: clockwise rotation degree from -360 - 360" << std::endl;
        std::cout << "    - opacity: transparancy percentage, 1 - 100, 0 is equal to 100, fully opaque" << std::endl;
        std::cout << "Material format for text/time:" << std::endl;
        std::cout << "  <type>:<layer>:<text|time_format>:<top>:<left>:<width>:<height>:<fontsize>:<rotation>:<opacity>:<font>:<color>:<outlinesize>:<outlinecolor>" << std::endl;
        std::cout << "    - type: text or time" << std::endl;
        std::cout << "    - time_format: same as strftime() format, use %20% for space, %c% for ':', see https://www.runoob.com/cprogramming/c-function-strftime.html" << std::endl;
        std::cout << "    - please see --watermark for explaination of other parameters" << std::endl;
        std::cout << "Material format for clock(analog clock):" << std::endl;
        std::cout << "  <type>:<layer>:<image_path_list>:<top>:<left>:<width>:<height>" << std::endl;
        std::cout << "    - type: clock " << std::endl;
        std::cout << "    - image_path_list: image path list separated by ',', must have exactly four images: " << std::endl;
        std::cout << "                       (0) clock background; (1) hour pointer; (2) minute pointer; (3) second pointer." << std::endl;
        std::cout << "                       All pointers must point to 00:00:00, and will be rotated by center point." << std::endl;
        std::cout << "Material format for product:(a group a set of materials, enabling batch switching by command)" << std::endl;
        std::cout << "  product:<product_id>:<material_id>:<material_spec>" << std::endl;
        std::cout << "     - product_id: an integer starting at 1, if it is 0, the material is used for all products" << std::endl;
        std::cout << "     - material_id: an integer start at 1 to uniquely specify a material, it is used to delete by command" << std::endl;
        std::cout << "     - material_spec: see Material format for media/text" << std::endl;
        std::cout << std::endl;
        std::cout << "Note:" << std::endl;
        std::cout << "Generally there should only be one mainvideo and/or mainaudio, which decides the length of the output video," << std::endl;
        std::cout << "but for streaming, mainvideo/mainaudio can be abscent, in which case the decorate loop will go on forever." << std::endl;
        std::cout << "The material's width/height will be used to resize itself if non zero." << std::endl;
        return -1;
    }

    bool enable_chromakeying = false;
    bool disable_opengl = false;
    bool enable_window = false;
    bool enable_ff_nv_enc = false;
    int enable_bg_color = 0;
    const char *scale_engine = "opengl";
    const char *scale_prefer = "speed";
    const char *encode_preset = "";
    const char *rawdata_out = NULL;
    const char *subtitle = NULL;
    cv::Rect subtitle_rect(0, 0, 0, 0);
    const char *subtitle_style = NULL;
    int force_width = 0, force_height = 0;
    std::vector<uchar> bg_bgr(3);
    rawvideoinfo rawvideo = { .pix_fmt=NULL, .w=1080, .h=1920, .fps=25, .bitrate=8000000 };
    rawaudioinfo rawaudio = { .aud_fmt=NULL, .channel=1, .samplerate=48000 }; // default output channel/sample_rate
    const char *rtmpout = NULL;
    const char *event_fifo = NULL;
    watermark water = {NULL};
    std::shared_ptr<IWaterMark> blind_watermark = nullptr;
    const char *use_ffmpeg = NULL;
    bool disable_ffmpeg_stat = false;
    const char *alpha_video = NULL; // left, right, top, bottom
    const char *alpha_engine = "opengl"; // opencv, opengl
    const char *stream_cmd_fifo = NULL, *stream_cmd_txtfile = NULL;
    int read_timeout = 100; // 100 seconds
    int stream_buffer_size = 10;


    for(int i=0; i<argc; i++)
    {
        const char *opt;
        int optlen;
        printf("[arg %d] %s\n", i, argv[i]);

        if(strcasecmp(argv[i], "--enable_chromakeying")==0)
        {
            enable_chromakeying = true;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        if(strcasecmp(argv[i], "--disable_opengl")==0)
        {
            disable_opengl = true;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        if(strcasecmp(argv[i], "--enable_window")==0)
        {
            enable_window = true;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        if(strcasecmp(argv[i], "--enable_debug")==0)
        {
            enable_debug = 1;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        if(strcasecmp(argv[i], "--enable_ff_nv_enc")==0)
        {
            enable_ff_nv_enc = true;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }

        opt = "--encode_preset=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            encode_preset = argv[i]+optlen;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--alpha_engine=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            alpha_engine = argv[i]+optlen;
            if (strncasecmp(alpha_engine, "opengl", 7) != 0 && strncasecmp(alpha_engine, "opencv", 7) != 0)
            {
                std::cerr << "Bad argument, invalid alpha_engine, must be one of opengl or opencv." << std::endl;
                return -1;
            }
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--read_timeout=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            read_timeout = atoi(argv[i]+optlen);
            if (read_timeout <= 0)
                read_timeout = 1;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--subtitle=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            std::vector<char *> strs = splitCString(argv[i]+optlen, ':');
            if (strs.size() < 6)
            {
                cerr << "Bad argument, invalid subtitle, format: --subtitle=file:top:left:width:height:style" << endl;
                return -1;
            }
            subtitle = strs[0];
            subtitle_rect.y = atoi(strs[1]);
            subtitle_rect.x = atoi(strs[2]);
            subtitle_rect.width = atoi(strs[3]);
            subtitle_rect.height = atoi(strs[4]);
            subtitle_style = strs[5];

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--use_ffmpeg=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            use_ffmpeg = argv[i]+optlen;
            if (!file_exists(use_ffmpeg))
            {
                std::cerr << "Error: --use_ffmpeg program file is missing" << std::endl;
                return -1;
            }
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--disable_ffmpeg_stat";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            disable_ffmpeg_stat = true;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }

        opt = "--notify_fifo_event=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            event_fifo = argv[i]+optlen;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--stream_cmd_fifo=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            stream_cmd_fifo = argv[i]+optlen;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--stream_cmd_txtfile=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            stream_cmd_txtfile = argv[i]+optlen;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--stream_buffer_size=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            stream_buffer_size = atoi(argv[i]+optlen);
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--bg_color=#";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            enable_bg_color = true;
            parse_color(argv[i]+optlen, bg_bgr);
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        //--alpha_video=left|right|top|bottom
        opt = "--alpha_video=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            alpha_video = argv[i]+optlen;
            if (strncasecmp(alpha_video, "left", 5) != 0 &&
                strncasecmp(alpha_video, "right", 6) != 0 &&
                strncasecmp(alpha_video, "top", 4) != 0 &&
                strncasecmp(alpha_video, "bottom", 7) != 0)
            {
                std::cerr << "Error: alpha_video parameter error, format: --alpha_video=left|right|top|bottom" << std::endl;
                return -1;
            }

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--watermark=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            memset(&water, 0, sizeof(water));
            const char *wmfmt =  "--watermark=text:font:size:#rgbcolor:rotation:opacity:rows:cols:top:left:width:height:#olcolor";
            std::vector<char *> strs = splitCString(argv[i]+optlen, ':');
            if (strs.size() < 6)
            {
                std::cerr << "Invalid watermark parameter, fmt: " << wmfmt << endl;
                return -1;
            }
            water.text = strs[0];
            water.font = strs[1];
            // replace %20% with space
            char *pspace = strstr(water.font, "%20%");
            while(pspace)
            {
                *pspace++ = ' ';
                memmove(pspace, pspace+3, strlen(pspace+3)+1);
                pspace = strstr(pspace, "%20%");
            }
            water.size = atoi(strs[2]);
            if (strs[3][0] != '#')
            {
                std::cerr << "Invalid watermark color: " << strs[3] << " (must be in format #rrggbb)" << endl;
                return -1;
            }
            std::vector<unsigned char> bgr;
            parse_color(strs[3]+1, bgr);
            water.bgr[0] = bgr[0];
            water.bgr[1] = bgr[1];
            water.bgr[2] = bgr[2];
            water.rotation = atoi(strs[4]);
            water.opacity = atoi(strs[5]);
            if (strs.size() >= 7)
                water.rows = atoi(strs[6]);
            if (strs.size() >= 8)
                water.cols = atoi(strs[7]);
            if (strs.size() >= 9)
                water.y = atoi(strs[8]);
            if (strs.size() >= 10)
                water.x = atoi(strs[9]);
            if (strs.size() >= 11)
                water.w = atoi(strs[10]);
            if (strs.size() >= 12)
                water.h = atoi(strs[11]);
            if (strs.size() >= 13)
            {
                std::vector<unsigned char> bgr;
                parse_color(strs[12]+1, bgr);
                water.olbgr[0] = bgr[0];
                water.olbgr[1] = bgr[1];
                water.olbgr[2] = bgr[2];
                water.olbgr[3] = 1;
            }

            if (water.rotation < -360 || water.rotation > 360 || water.opacity < 0 || water.opacity > 100)
            {
                std::cerr << "Invalid watermark parameters, rotation [-360, 360], opacity [0, 100]\n" << endl;
                return -1;
            }
            if (water.opacity==0) // 0 is equal to 100
                water.opacity = 100;

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--blind_watermark=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            auto blind_watermark_options = argv[i]+optlen;
            auto watermark = IWaterMark::create(blind_watermark_options);
            if (!watermark)
            {
                std::cerr << "Invalid blind_watermark parameter: " << blind_watermark_options << std::endl;
                return -1;
            }
            blind_watermark.reset(watermark);
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--scale_engine=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            scale_engine = argv[i]+optlen;
            if (strncasecmp(scale_engine, "ffmpeg", 7)!=0 &&
                strncasecmp(scale_engine, "opencv", 7)!=0 &&
                strncasecmp(scale_engine, "opengl", 7)!=0)
            {
                std::cerr << "Invalid scale_engine parameter: " << scale_engine << std::endl;
                return -1;
            } 
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--scale_prefer=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            scale_prefer = argv[i]+optlen;
            if (strncasecmp(scale_prefer, "quality", 8)!=0 &&
                strncasecmp(scale_prefer, "speed", 6)!=0 &&
                strncasecmp(scale_prefer, "both", 5)!=0)
            {
                std::cerr << "Invalid scale_prefer parameter: " << scale_prefer << std::endl;
                return -1;
            }

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--video_size=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            char *szstr = argv[i]+optlen;
            char *pos = strchr(szstr, 'x');
            if (pos == NULL)
            {
                std::cerr << "Invalid video_size parameter: " << szstr << std::endl;
                return -1;
            }
            *pos++ = 0;
            force_width = atoi(szstr);
            force_height = atoi(pos);
            if (force_width < 10 || force_height < 10)
            {
                std::cerr << "Invalid video_size specified: [" << force_width << ", " << force_height << "]" << std::endl;
                return -1;
            }

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--rawvideo=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            // pix_fmt:width:height:fps:bitrate
            std::vector<char *> strs = splitCString(argv[i]+optlen, ':');
            for(int idx=0; idx<strs.size(); idx++)
            {
                char *val = strs[idx];
                switch(idx)
                {
                case 0:
                    rawvideo.pix_fmt = val;
                    break;
                case 1:
                    rawvideo.w = atoi(val);
                    break;
                case 2:
                    rawvideo.h = atoi(val);
                    break;
                case 3:
                    rawvideo.fps = atoi(val);
                    break;
                case 4:
                    rawvideo.bitrate = atoi(val);
                    break;
                default:
                    break;
                }
            }

            if (rawvideo.pix_fmt==NULL ||
             (strncasecmp(rawvideo.pix_fmt, "bgr", 4)!=0 && strncasecmp(rawvideo.pix_fmt, "bgra", 5)!=0 &&
              strncasecmp(rawvideo.pix_fmt, "rgb", 4)!=0 && strncasecmp(rawvideo.pix_fmt, "rgba", 5)!=0 &&
              strncasecmp(rawvideo.pix_fmt, "yuv420p", 8)!=0) ||
                rawvideo.w==0 || rawvideo.h==0 || rawvideo.fps < 10)
            {
                std::cerr << "Invalid rawvideo parameter: pix_fmt" << rawvideo.pix_fmt << ", width=" << rawvideo.w << ", height=" << rawvideo.h << ", fps=" << rawvideo.fps << std::endl;
                return -1;
            }
            if (rawvideo.bitrate < 1000000) // 0 for default of 8M, otherwise set lower bound to 1M
            {
                rawvideo.bitrate = rawvideo.bitrate>0? 1000000 : 8000000;
            }
            if (rawvideo.bitrate > 50000000) // some bgr video may have *G of bitrate, set upper bound to 50M
            {
                rawvideo.bitrate = 50000000;
            }

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--rawaudio=";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            // aud_fmt:samplerate:channelnum
            std::vector<char *> strs = splitCString(argv[i]+optlen, ':');
            for(int idx=0; idx<strs.size(); idx++)
            {
                char *val = strs[idx];
                switch(idx)
                {
                case 0:
                    rawaudio.aud_fmt = val;
                    break;
                case 1:
                    rawaudio.samplerate = atoi(val);
                    break;
                case 2:
                    rawaudio.channel = atoi(val);
                    break;
                default:
                    break;
                }
            }

            if (rawaudio.aud_fmt==NULL || strncasecmp(rawaudio.aud_fmt, "pcm_s16le", 10)!=0 ||
                rawaudio.samplerate<1000 || rawaudio.channel < 1)
            {
                cerr << "Invalid rawaudio parameter: aud_fmt" << rawaudio.aud_fmt << ", samplerate=" << rawaudio.samplerate << ", channel_num=" << rawaudio.channel << endl;
                return -1;
            }

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        opt = "--stream_out=rtmp://";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            rtmpout = argv[i]+strlen("--stream_out=");
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
        // --substream_out=protocol://proto_spec:<width>:<height>:<framerate>:<bitrate>:<skipaudio>
        opt = "--substream_out=rtmp://";
        optlen = strlen(opt);
        if(strncasecmp(argv[i], opt, optlen)==0)
        {
            substream_out.rtmpout = argv[i]+strlen("--substream_out=");
            char *lastslash = strrchr(substream_out.rtmpout, '/');
            char *param = lastslash? strchr(lastslash, ':') : NULL;
            if (param == NULL)
            {
                std::cerr << "Invalid substream out parameters" << std::endl;
                return -1;
            }
            *param ++ = 0;
            std::vector<char *> strs = splitCString(param, ':');
            if (strs.size() < 4)
            {
                std::cerr << "Invalid substream_out parameters" << std::endl;
                return -1;
            }
            substream_out.videoinfo.w = atoi(strs[0]);
            substream_out.videoinfo.h = atoi(strs[1]);
            substream_out.videoinfo.fps = atoi(strs[2]);
            substream_out.videoinfo.bitrate = atoi(strs[3]);
            substream_out.disable_audio = strs.size() >= 5? atoi(strs[4]) : 0;

            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }

        // command line backward compatible
        opt = "--";
        optlen = strlen(opt);
        // argv[i] len should > 2, because -- has special meaning
        if (strncasecmp(argv[i], opt, optlen)==0 && optlen < strlen(argv[i]))
        {
            std::cerr << "Unrecognized option: " << argv[i] << std::endl;
            if(i+1 < argc)
                memmove(argv+i, argv+i+1, (argc-i-1)*sizeof(argv));
            --argc;
            --i;
            continue;
        }
    }

    if(alpha_video) // no greenmatting on alpha video
        enable_chromakeying = false;

    string ffmpeg = use_ffmpeg? std::string(use_ffmpeg) : get_ffmpeg_path();
    if(ffmpeg.empty())
    {
        return -1; 
    }

    // parse output file/width/height/[fps]/[bitrate]/[format]
    int output_width = 0;
    int output_height = 0;
    int output_fps = 0;
    int output_bitrate = 0;
    char *output_fmt = (char*)"mp4"; // currently support mp4, mov, webm, raw24, raw32
    char outputvideo[1024];
    char eventfile[1024];
    bool output_alpha = false;
    char *ov = argv[1];
    bool has_stream_io = false; // if input/output has stream, we need to collate all streams time with mainvideo stream

    std::vector<char *> strs = splitCString(ov, ':');
    if (strs.size() < 3)
    {
        std::cerr << "Invalid output video format: " << ov << std::endl;
        return -1;
    }
    output_width = atoi(strs[1]);
    output_height = atoi(strs[2]);
    if (strs.size() >= 4)
        output_fps = atoi(strs[3]);
    if (strs.size() >= 5)
        output_bitrate = atoi(strs[4]);
    if (strs.size() >= 6)
        output_fmt = strs[5];
    if(output_width <= 0 || output_height <= 0)
    {
        if (rawvideo.pix_fmt && rawvideo.w && rawvideo.h)
        {
            int a = alpha_video? tolower(alpha_video[0]) : 0;
            output_width = (a == 'l' || a == 'r')? rawvideo.w * 2 : rawvideo.w;
            output_height = (a == 't' || a == 'b')? rawvideo.h * 2 : rawvideo.h;;
            std::cout << "Warning: output video size is 0, will use rawvideo's size of [" << rawvideo.w << "x" << rawvideo.h << "]!" << std::endl;
        }
        else
        {
            std::cerr << "Invalid output video width (" << output_width << ") or height (" << output_height << ")" << std::endl;
            return -1;
        }
    }
    if(strncasecmp(output_fmt, "mp4", 4)!=0 && strncasecmp(output_fmt, "mov", 4)!=0 &&
         strncasecmp(output_fmt, "webm", 5)!=0 && strncasecmp(output_fmt, "raw24", 6)!=0 &&
         strncasecmp(output_fmt, "raw32", 6)!=0 && strncasecmp(output_fmt, "mp4alpha", 9)!=0)
    {
        std::cerr << "Invalid output video format: " << output_fmt << "!" << std::endl;
        return -1;
    }
    output_alpha = (strncasecmp(output_fmt, "mov", 4)==0 || strncasecmp(output_fmt, "webm", 5)==0 ||
                    strncasecmp(output_fmt, "raw32", 6)==0 || strncasecmp(output_fmt, "mp4alpha", 9)==0);
    merge_path(outputvideo, sizeof(outputvideo), data_dir, ov);
    if(event_fifo)
    {
        merge_path(eventfile, sizeof(eventfile), data_dir, event_fifo);
        event_fifo = eventfile;
    }
    if (strncasecmp(output_fmt, "raw", 3)==0) // output raw data
        rawdata_out = outputvideo;
    if (rawdata_out && rtmpout)
    {
        std::cerr << "Invalid argument: it is impossible to output raw data to stream!" << std::endl;
        return -1;
    }
    std::cout << "Output video path: " << outputvideo << std::endl;
    double x_ratio = 1.0, y_ratio = 1.0; // scale ratio for all materials
    if (force_width && force_height)
    {
        x_ratio = ((double)force_width) / output_width;
        y_ratio = ((double)force_height) / output_height;
        output_width = force_width;
        output_height = force_height;
    }
    material mainvideo, mainaudio;
    std::vector<material> mlist;
    auto ret = parse_materials(argc, argv, mainvideo, mainaudio, mlist, data_dir, x_ratio, y_ratio);
    if (ret)
        return -1;

    if (event_fifo && (ret = init_fifo_event(event_fifo)) < 0)
    {
        std::cerr << "Error: failed to init fifo event on file " << event_fifo << std::endl;
        return -1;
    }

    int fps = output_fps? output_fps : 15;
    has_stream_io = check_has_stream(mlist) || rtmpout;
    int bitrate = output_bitrate? output_bitrate : 2000000;
    int64_t duration = 7LL*24*3600*1000; // output video duration in ms, maximum of 1 week
    int64_t totalframes = 0;
    FFReader *ffreader = NULL;
    if(mainvideo.type == material::MT_MainVideo)
    {
        // open mainvideo
        if (rawvideo.pix_fmt)
        {
            ffreader = open_raw_video(mainvideo.path, 0, 0, NULL,
                                        rawvideo.pix_fmt, rawvideo.pix_fmt, rawvideo.w, rawvideo.h, rawvideo.fps, rawvideo.bitrate,
                                        rawaudio.aud_fmt, rawaudio.channel, rawaudio.samplerate);
        }
        else
        {
            auto out_fmt = AV_PIX_FMT_BGRA;
            size_t l;
            if (alpha_video || ((l=strlen(mainvideo.path)) > 4 && strcasecmp(mainvideo.path+l-4, ".mp4")==0))
                out_fmt = AV_PIX_FMT_BGR24;

            // open mainvideo, always convert color to bgra
            if(strncasecmp(scale_engine, "ffmpeg", 6)==0 && alpha_video==NULL) // scale with ffmpeg
                ffreader = open_video_reader(mainvideo.path, true, true, mainvideo.rect.width, mainvideo.rect.height, 
                                             scale_prefer, out_fmt, AV_SAMPLE_FMT_S16, rawaudio.channel, rawaudio.samplerate);
            else // no scaling
                ffreader = open_video_reader(mainvideo.path, true, true, 0, 0, 
                                             NULL, out_fmt, AV_SAMPLE_FMT_S16, rawaudio.channel, rawaudio.samplerate);
        }
        if(ffreader == NULL)
        {
            if (check_is_stream(mainvideo))
                send_event(ET_STREAM_NONEXIST, "Mainvideo stream nonexists");
            std::cerr << "Error opening main video file " << mainvideo.path << std::endl;
            return -1;
        }

        materialcontext &ctx = mainvideo.ctx;
        ctx.w = ffreader->disp_width;
        ctx.h = ffreader->disp_height;
        int a = alpha_video? tolower(alpha_video[0]) : 0;
        if (a == 'l' || a == 'r') // left-right video
        {
            ctx.w = ctx.w / 2;
        }
        if (a == 't' || a == 'b') // top-bottom video
        {
            ctx.h = ctx.h / 2;
        }
        if(mainvideo.rect.width==0)
            mainvideo.rect.width = ctx.w;
        if(mainvideo.rect.height==0)
            mainvideo.rect.height = ctx.h;

        ctx.fps = ffreader->fps;
        if (!output_fps)
            fps = (int)(ffreader->fps + 0.5);
        if (!output_bitrate)
        {
            bitrate = ffreader->bitrate;
            if (bitrate < 1000000)
                bitrate = 1000000;
            // limit streaming bitrate
            if (rtmpout && bitrate > 6000000)
                bitrate = 6000000;
        }
        duration = ffreader->totaltime;
    }
    else if (mainaudio.type == material::MT_MainAudio) // mainaudio
    {
        // open mainaudio rawdata fifo
        if (rawaudio.aud_fmt)
        {
            mainaudio.ctx.reader = ffreader = open_raw_video(mainaudio.path, 0, 0, NULL, NULL, NULL, 0, 0, fps, 0,
                                    rawaudio.aud_fmt, rawaudio.channel, rawaudio.samplerate);
            if(ffreader == NULL)
            {
                std::cerr << "Error opening raw audio file " << mainaudio.path << std::endl;
                return -1;
            }
            duration = 7LL*60*60*24*1000;
        }
        else
        {
            ffreader = mainaudio.ctx.reader;
            if(mainaudio.ctx.reader)
                duration = mainaudio.ctx.reader->totaltime;
            else
                duration = get_video_duration(mainaudio.path);
            if(duration <= 0)
            {
                std::cerr << "Error getting duration for main audio file: " << mainaudio.path << std::endl;
                return -1;
            }
        }
        enable_chromakeying = false;
    }
    totalframes = duration / (1000.0 / fps);
    printf("output video fps : %d, total frames: %lld\n", fps, (long long)totalframes);
    FILE *writer_pipe = NULL;
    out_audio_fifoname = outputvideo+string(".")+std::to_string(time(NULL))+string(".fifo");

    // create dir if not exist
    if (ensure_dir_exists(outputvideo) < 0)
        return -1;

    int product_id = -1;
    // find smallest valid product id, as the beginning default product id
    for (auto &m : mlist)
    {
        if (m.product_id > 0)
        {
            if (product_id < 0 || m.product_id < product_id)
                product_id = m.product_id;
        }
    }

    // open all materials
    ret = open_materials(mlist, x_ratio, y_ratio, &rawaudio, &mainaudio, stream_buffer_size, fps, disable_opengl, product_id);
    if (ret)
        return -2;

    // open write file
    std::string cmd;
    int output_w = output_width;
    int output_h = output_height;
    if (rawdata_out)
    {
        writer_pipe = fopen(rawdata_out, "w");
        if(!writer_pipe)
        {
            std::cerr << "Error opening rawdata file " << rawdata_out << ", err=" << errno << ":" << strerror(errno) << std::endl;
            return -1;
        }
        ffVideoEncodeThread.START(writer_pipe);
    }
    else
    {
        if (strncasecmp(output_fmt, "mp4alpha", 9) == 0)
        {
            if (output_width > output_height)
            {
                output_h *= 2;
            }
            else
            {
                output_w *= 2;
            }
        }

        ret = mkfifo(out_audio_fifoname.c_str(), S_IRUSR|S_IWUSR);
        if (ret)
        {
            std::cerr << "Error creating fifo for sending audio data: errno=" << errno << ":" << strerror(errno) << std::endl;
            return -1;
        }
        cmd = format_ffmepg_encode_cmdline(ffmpeg, outputvideo, output_w, output_h,
                fps, bitrate, enable_ff_nv_enc, encode_preset, output_fmt,
                out_audio_fifoname.c_str(), rawaudio.channel, rawaudio.samplerate, rtmpout, disable_ffmpeg_stat);
        writer_pipe = popen(cmd.c_str(), "w");
        if(!writer_pipe)
        {
            remove(out_audio_fifoname.c_str());
            std::cerr << "Error executing ffmpeg, err=" << errno << ":" << strerror(errno) << ", cmd:\n\t" << cmd << std::endl;
            return -1;
        }
        std::cout << "Executing ffmpeg, cmd:\n\t" << cmd << std::endl;
        ffVideoEncodeThread.START(writer_pipe);
    }

    if (stream_cmd_fifo || stream_cmd_txtfile)
    {
        ret = start_stream_cmd_thread(stream_cmd_fifo? stream_cmd_fifo : stream_cmd_txtfile, stream_cmd_fifo? false : true);
        if (ret < 0)
        {
            std::cout << "Failed to start command fifo thread." << std::endl;
            return -1;
        }
    }

    if (!rawdata_out)
    {
        ffAudioEncodeThread.START(out_audio_fifoname);
    }
    substream_out.audioinfo = rawaudio;
    if (substream_out.rtmpout)
    {
        if (!substream_out.disable_audio)
        {
            auto str = outputvideo+string(".substream.")+std::to_string(time(NULL))+string(".fifo");
            strncpy(substream_out.audio_fifo, str.c_str(), sizeof(substream_out.audio_fifo));
        }
        if (substream_out.videoinfo.fps > fps)
            substream_out.videoinfo.fps = fps;
        start_streamout_thread(&substream_out, ffmpeg);
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, sig_handler);
    signal(SIGQUIT, sig_handler);
    signal(SIGKILL, sig_handler);
    signal(SIGABRT, sig_handler);
    // signal(SIGSEGV, sig_handler); // let the system generate coredump
    signal(SIGUSR1, sig_handler);

    if (!disable_opengl)
    {
        const char *gl_prefer = strncasecmp(scale_engine, "opengl", 6)==0? scale_prefer : "speed";
        int r = gl_init_render(output_width, output_height, enable_debug, gl_prefer, output_alpha);
        if (r)
        {
            remove(out_audio_fifoname.c_str());
            std::cout << "Error initializing opengl rendering engine." << std::endl;
            return -1;
        }
    }

    // display a test window
    if(enable_window)
    {
        namedWindow("Decorated Video", WINDOW_NORMAL);
    }

    GetLogHelp().Init(data_dir, enable_debug? LogLevel::debug : LogLevel::info);

    {
    AUTOTIME("MergeVideo Run");

    double ts = 0.0;
    int num = -1;
    Mat bgmat;

    // fill background color
    if(enable_bg_color)
    {
        if (output_alpha)
            bgmat = cv::Mat(cv::Size(output_width, output_height), CV_8UC4, cv::Scalar(bg_bgr[0], bg_bgr[1], bg_bgr[2], 255));
        else
            bgmat = cv::Mat(cv::Size(output_width, output_height), CV_8UC3, cv::Scalar(bg_bgr[0], bg_bgr[1], bg_bgr[2]));
    }
    else if (output_alpha)
    {
        bgmat = cv::Mat::zeros(cv::Size(output_width, output_height), CV_8UC4);
    }
    else if (!disable_opengl)
    {
        bgmat = cv::Mat::zeros(cv::Size(output_width, output_height), CV_8UC3);
    }

    // process static images
    int static_idx = 0;
    for(; output_alpha==false && static_idx<mlist.size(); static_idx++)
    {
        auto &m = mlist[static_idx];
    
        if (m.product_id > 0)
            break;
        if(m.type == material::MT_MainAudio || m.type == material::MT_Audio) // skip audios
            continue;

        if(!disable_opengl && (m.rotation > 0 || (m.opacity > 0 && m.opacity < 100))) // pass on to opengl
            break;

        // static image only
        if(m.type == material::MT_Gif || m.type == material::MT_Video ||
             m.type == material::MT_MainVideo || m.type == material::MT_Clock)
            break;

        cv::Mat *pmat, mask;
        pmat = read_next_frame(m, ts, disable_opengl);
        if(!pmat || pmat->empty())
        {
            LOG_ERROR("Error reading frame from material %s", m.path);
            continue;
        }

        OverlapImage(m, bgmat, output_width, output_height, *pmat, mask, output_alpha);
    }

    unsigned int bgTexture = 0;
    unsigned int waterTexture = 0;
    unsigned int subTexture = 0;
    Mat base = cv::Mat::zeros(cv::Size(output_width, output_height), output_alpha? CV_8UC4 : CV_8UC3);
    cv::Mat yuv(cv::Size(output_width, output_height+output_height/2), CV_8U);
    cv::Mat watermat;
    if (water.text)
    {
        if (water.rows == 0.0)
            water.rows = output_height > output_width? 5 : 6;
        if (water.cols == 0.0)
            water.cols = output_height > output_width? 3 : 6;
        bool repeat = (water.rows != 1 && water.cols != 1);
        int water_width = repeat? output_width / water.cols : water.w;
        int water_height = repeat? output_height / water.rows : water.h;
        cv::Point startPos(20, 20);
        auto mat = text2Mat(water.text, water.font, water.size * 2,
                 cv::Scalar(water.bgr[0], water.bgr[1], water.bgr[2], ((double)water.opacity)*255/100),
                 startPos, water_width, water_height, repeat? cvx::WRAP_CROP : cvx::WRAP_ALIGN_CENTER, 
                 water.olbgr[3]? cv::Scalar(water.olbgr[0], water.olbgr[1], water.olbgr[2], ((double)water.opacity)*255/100) : cv::Scalar(0,0,0,0));
        if (mat.empty()) // probably failed to load font
        {
            remove(out_audio_fifoname.c_str());
            return -1;
        }
        if (water_width < mat.cols)
            water_width = mat.cols;
        if (water_height < mat.rows)
            water_height = mat.rows;
        watermat = cv::Mat(cv::Size(water_width, water_height), CV_8UC4, cv::Scalar::all(0));
        mat.copyTo(watermat(cv::Rect((water_width-mat.cols)/2, (water_height-mat.rows)/2, mat.cols, mat.rows)));
    }

    if (subtitle)
    {
        int subtitle_fps = 8;
        ret = ffSubtitleEncodeThread.START(subtitle, subtitle_style, subtitle_fps, output_width, output_height,
                                subtitle_rect.x, subtitle_rect.y, subtitle_rect.width, subtitle_rect.height);
        if (ret < 0)
        {
            LOG_ERROR("Subtitle encoder failed to load file %s", subtitle);
            remove(out_audio_fifoname.c_str());
            return -1;
        }
        std::cout << "Subtitle info: align = " << ffSubtitleEncodeThread.align << std::endl;
        std::cout << "                   x = " << ffSubtitleEncodeThread.sub_x << std::endl;
        std::cout << "                   y = " << ffSubtitleEncodeThread.sub_y << std::endl;
        std::cout << "                   w = " << ffSubtitleEncodeThread.sub_w << std::endl;
        std::cout << "                   h = " << ffSubtitleEncodeThread.sub_h << std::endl;
    }

    if(event_fifo)
    {
        send_event(ET_INIT_SUCCESS, "init success");
    }

    if(mainvideo.type == material::MT_MainVideo)
    {
        start_video_decoder_thread(ffreader, AV_TOGETHER, check_is_stream(mainvideo)? stream_buffer_size : 0);
    }
    else if(mainaudio.type == material::MT_MainAudio && rawaudio.aud_fmt)
    {
        start_video_decoder_thread(ffreader, AV_SEPARATE, 0);
    }

    std::chrono::steady_clock::time_point now, wish = std::chrono::steady_clock::now();
    std::chrono::nanoseconds timebase((long long)(1000000000LL / fps));
    std::chrono::nanoseconds dns(3000000), max_dns(-100000000LL);
    int timeout_num = 0;
    bool first_frame_ready = false;
    int sent_audio_bytes = 0; // bytes of audio already sent
    // logically each frame's audio size
    double audio_per_frame = ((double)(rawaudio.samplerate * 2 * rawaudio.channel)) / fps;
    bool first_run = true;

    while(++num >= 0)
    {
        if(mainvideo.type != material::MT_MainVideo)
        {
            if (num >= totalframes)
                break;
        }

        std::vector<stream_cmd_info> cmds;
        get_stream_cmds(cmds);
        bool modified = false;
        if (cmds.size())
        {
            std::vector<material> new_mlist;
            for (auto &c : cmds)
            {
                if (c.operation != stream_cmd_info::SUBOUT && c.operation != stream_cmd_info::STOPSUB &&
                    c.operation != stream_cmd_info::SWPROD && c.material_id < 1)
                {
                    cerr << "Warning: invalid material_id in stream command, operation: " << c.operation << ", product_id: " << c.product_id << ", material_id: " << c.material_id << endl;
                    continue;
                }
                switch (c.operation)
                {
                case stream_cmd_info::ADD:
                    {
                        material m;
                        if (parse_material(c.material.c_str(), m, data_dir, x_ratio, y_ratio))
                        {
                            break;
                        }
                        if (m.type == material::MT_MainVideo)
                            m.type = material::MT_Video;
                        if (m.type == material::MT_MainAudio)
                            m.type = material::MT_Audio;
                        m.product_id = c.product_id;
                        m.material_id = c.material_id;
                        new_mlist.push_back(m);
                        break;
                    }
                case stream_cmd_info::DEL:
                    {
                        for (int i=0; i<mlist.size(); i++)
                        {
                            auto &m = mlist[i];
                            if ((c.product_id <=0 || m.product_id == c.product_id) && m.material_id == c.material_id)
                            {
                                // close material m
                                if (m.type == material::MT_MainVideo || m.type == material::MT_MainAudio)
                                {
                                    cerr << "Error: could not delete " << (m.type == material::MT_MainVideo? "mainvideo" : "mainaudio") << " by stream command, product_id: " << c.product_id << ", material_id: " << c.material_id << endl;
                                    break;
                                }
                                if ((!disable_opengl) && m.ctx.glTexture)
                                {
                                    gl_delete_texture(m.ctx.glTexture);
                                }
                                send_event(ET_MATERIAL_DEL_SUCC, std::string("Delete material ")+m.path+" successfully");
                                close_material(m);
                                mlist.erase(mlist.begin()+i);
                                break;
                            }
                        }
                        break;
                    }
                case stream_cmd_info::MOD:
                    {
                        for (int i=0; i<mlist.size(); i++)
                        {
                            auto &m = mlist[i];
                            if ((c.product_id <=0 || m.product_id == c.product_id) && m.material_id == c.material_id)
                            {
                                if (m.type == material::MT_MainAudio || m.type == material::MT_Audio)
                                    break;
                                m.layer = c.layer;
                                m.rect.x = c.rect.x * x_ratio;
                                m.rect.y = c.rect.y * y_ratio;
                                m.rect.width = c.rect.w * x_ratio;
                                m.rect.height = c.rect.h * y_ratio;
                                send_event(ET_MATERIAL_MOD_SUCC, std::string("Modify material ")+m.path+" successfully");
                                modified = true;
                                break;
                            }
                        }
                        break;
                    }
                case stream_cmd_info::SUBOUT:
                    {
                        strncpy(substream_out.streamspec, c.material.c_str(), sizeof(substream_out.streamspec)-1);
                        substream_out.streamspec[sizeof(substream_out.streamspec)-1] = 0;
                        substream_out.rtmpout = substream_out.streamspec;
                        char *lastslash = strrchr(substream_out.rtmpout, '/');
                        char *param = lastslash? strchr(lastslash, ':') : NULL;
                        if (param)
                        {
                            *param ++ = 0;
                            std::vector<char *> strs = splitCString(param, ':');
                            if (strs.size() < 4)
                            {
                                std::cerr << "Invalid substream_out parameters" << std::endl;
                                return -1;
                            }
                            substream_out.videoinfo.w = atoi(strs[0]);
                            substream_out.videoinfo.h = atoi(strs[1]);
                            substream_out.videoinfo.fps = atof(strs[2]);
                            substream_out.videoinfo.bitrate = atoi(strs[3]);
                            substream_out.disable_audio = strs.size() >= 5? atoi(strs[4]) : 0;
                            if (substream_out.videoinfo.fps > fps)
                                substream_out.videoinfo.fps = fps;
                            if (!substream_out.disable_audio)
                            {
                                if (substream_out.audio_fifo[0]) // remove old fifo
                                    remove(substream_out.audio_fifo);
                                auto str = outputvideo+string(".substream.")+std::to_string(time(NULL))+string(".fifo");
                                strncpy(substream_out.audio_fifo, str.c_str(), sizeof(substream_out.audio_fifo));
                            }
                            if (substream_out.video_thread && !substream_out.video_thread->bStopped)
                            {
                                std::cerr << "Error: Substream output is running, please stop first!" << std::endl;
                                break;
                            }
                            substream_out.start_frameno = num;
                            substream_out.cur_frameno = 0;
                            start_streamout_thread(&substream_out, ffmpeg);
                        }
                        break;
                    }
                case stream_cmd_info::STOPSUB:
                    stop_streamout_thread(&substream_out, true);
                    break;
                case stream_cmd_info::SWPROD:
                    if (c.product_id <= 0)
                    {
                        cerr << "Warning: invalid product_id of " << c.product_id << " in SWPROD operation" << endl;
                        break;
                    }
                    if (c.product_id > 0 && product_id != c.product_id)
                    {
                        // switch product
                        int old_product_id = product_id;
                        product_id = c.product_id;
                        cout << "Switch product from " << old_product_id << " to " << product_id << endl;

                        for (auto &m : mlist)
                        {
                            if (m.type == material::MT_Audio || m.type == material::MT_Video)
                            {
                                if (m.product_id > 0 && m.product_id == old_product_id) // non-current product, stop
                                    stop_video_decoder_thread(m.ctx.reader, AV_STREAMING, check_is_stream(m)? stream_buffer_size : 0);
                                else if (m.product_id > 0 && m.product_id == product_id) // new current product, start
                                    start_video_decoder_thread(m.ctx.reader, AV_STREAMING, check_is_stream(m)? stream_buffer_size : 0);
                            }
                        }
                    }
                    break;
                default:
                    cerr << "Stream command " << c.operation << " not supported." << endl;
                    break;
                }
            }
            if (!new_mlist.empty()) // submit to open in new thread
                submit_open_materials(new_mlist, x_ratio, y_ratio, rawaudio, mainaudio, stream_buffer_size, fps, disable_opengl, product_id);
        }
        // check open_materials thread result
        auto pmlist = check_open_materials();
        if (pmlist || modified)
        {
            if (pmlist)
            {
                mlist.insert(mlist.end(), pmlist->begin(), pmlist->end());
                delete pmlist;
            }
            std::sort(mlist.begin(), mlist.end(), 
                [](const material &a2, const material &a1)->bool{
                    return (a2.layer - a1.layer) < 0;
                });
        }

        AUTOTIMED("Frame handling total run", enable_debug);
        Mat frame, mask;
        int prodid = 0; // new product id
        std::vector<unsigned char> main_vdata, main_adata;
        // if main video is from shm, it is AI frame and possibly is delay, so set can_wait = true
        bool can_wait = first_frame_ready==false || has_stream_io==false || (mainvideo.type == material::MT_MainVideo && strncmp(mainvideo.path, "shm://", 6)==0);
        bool mainaudio_missing = false;
        int audio_bytes_cur_frame = audio_per_frame * (num+1) - sent_audio_bytes;
        if (audio_bytes_cur_frame & 1) audio_bytes_cur_frame += 1;

        // read main video frame
        if (mainvideo.type == material::MT_MainVideo)
        {
            AUTOTIMED("FFMPEG read frame run", enable_debug);
            // if user specified output fps, need to adjust frame output by pts
            if (ffreader->fps >= (double)fps + 0.1 || ffreader->fps <= (double)fps - 0.1)
            {
                bool use_old = true;
                while (mainvideo.ctx.frames.empty() || mainvideo.ctx.cts + mainvideo.ctx.tstep < ts)
                {
                    use_old = false;
                    std::vector<unsigned char> adata;
                    int ret = read_thread_merge_data(ffreader, main_vdata, adata, prodid, can_wait? 1 : 0);
                    if (ret < 0 || (can_wait && ret == 1 && timeout_num>=read_timeout))
                    {
                        auto estr = string("Read mainvideo ") + (ret > 0? "timeout" : "error");
                        send_event(ET_READ_FAILURE, estr);
                        LOG_ERROR("Error: %s!", estr.c_str());
                        remove(out_audio_fifoname.c_str());
                        return DEC_ERROR_BAD_STREAM_FORMAT;
                    }
                    if (ret == 1) // timeout
                    {
                        timeout_num ++;
                        break;
                    }
                    timeout_num = 0;
                    main_adata.insert(main_adata.end(), adata.begin(), adata.end());
                    if(main_vdata.empty())
                        break;
                    if(mainvideo.ctx.frames.empty())
                    {
                        mainvideo.ctx.cts = 0;
                        mainvideo.ctx.tstep = 1000.0 / ffreader->fps;
                        break;
                    }
                    mainvideo.ctx.cts += mainvideo.ctx.tstep;
                }
                if(main_vdata.empty() && use_old && !mainvideo.ctx.frames.empty())
                {
                    frame = mainvideo.ctx.frames[0].clone();
                }
                else if(!main_vdata.empty())
                {
                    frame = get_bgra_mat(ffreader, mainvideo, main_vdata.data(),
                                    AV_PIX_FMT_NONE, disable_opengl, disable_opengl? alpha_video : NULL, &mask);
                    if(mainvideo.ctx.frames.empty())
                        mainvideo.ctx.frames.push_back(frame.clone());
                    else
                        mainvideo.ctx.frames[0] = frame.clone();
                }
            }
            else
            {
                std::vector<unsigned char> adata;
                int ret = read_thread_merge_data(ffreader, main_vdata, adata, prodid, can_wait? 1 : 0);
                if (ret < 0 || (can_wait && ret == 1 && timeout_num>=read_timeout))
                {
                    auto estr = string("Read mainvideo ") + (ret > 0? "timeout" : "error");
                    send_event(ET_READ_FAILURE, estr);
                    remove(out_audio_fifoname.c_str());
                    return DEC_ERROR_BAD_STREAM_FORMAT;
                }
                if (ret == 1) // timeout
                {
                    timeout_num ++;
                }
                else
                {
                    timeout_num = 0;
                    main_adata.insert(main_adata.end(), adata.begin(), adata.end());
                    if(!main_vdata.empty())
                    {
                        frame = get_bgra_mat(ffreader, mainvideo, main_vdata.data(), 
                                AV_PIX_FMT_NONE, disable_opengl, disable_opengl? alpha_video : NULL, &mask);
                    }
                }
            }
            if (frame.empty() && can_wait && timeout_num > 0) // timeout_num==0 if no video but has audio
            {
                num --;
                continue;
            }
            if(!frame.empty())
            {
                // convert top/bottom, left/right video to alpha video
                if((!disable_opengl) && alpha_video && strncasecmp(alpha_engine, "opencv", 7)==0)
                {
                    AUTOTIMED("FFMPEG convert alpha image", enable_debug);
                    frame = MakeAlphaMat(frame, alpha_video[0], NULL);
                }
                if(((mainvideo.rotation%360)==0 && (mainvideo.opacity<=0 || mainvideo.opacity>=100)) && 
                    strncasecmp(scale_engine, "opencv", 6)==0 && // scale with opencv
                    (mainvideo.ctx.w!=mainvideo.rect.width ||
                    mainvideo.ctx.h!=mainvideo.rect.height))
                {
                    int flag = INTER_LINEAR;
                    if (scale_prefer && strncasecmp(scale_prefer, "quality", 7)==0)
                        flag = INTER_CUBIC;
                    else if (scale_prefer && strncasecmp(scale_prefer, "speed", 5)==0)
                        flag = INTER_NEAREST;
                    else if (scale_prefer && strncasecmp(scale_prefer, "both", 5)==0)
                        flag = INTER_LINEAR;
                    cv::resize(frame, frame, cv::Size(mainvideo.rect.width, mainvideo.rect.height), 0, 0, flag);
                }
            }
        }
        else if (mainaudio.type == material::MT_MainAudio && rawaudio.aud_fmt)
        {
            ret = read_thread_separate_audio_frame(ffreader, main_adata, prodid, can_wait? 1 : 0);
            if (ret < 0 || (can_wait && ret == 1 && timeout_num>=read_timeout))
            {
                send_event(ET_READ_FAILURE, "Read mainaudio error");
                LOG_ERROR("Read main audio frame error!");
                remove(out_audio_fifoname.c_str());
                return DEC_ERROR_BAD_STREAM_FORMAT;
            }
            if (can_wait && ret == 1)
            {
                timeout_num ++;
                num --;
                continue;
            }
            if (ret == 1 && audio_bytes_cur_frame > 1)
            {
                mainaudio_missing = true;
                LOG_INFO("Warning: main audio is missing one frame of data (%d bytes)", audio_bytes_cur_frame);
            }
            timeout_num = 0;
        }

        // skip image rendering if mainvideo EOF (has mainvideo && frame.empty)
        if (mainvideo.type != material::MT_MainVideo || !frame.empty()) // mainvideo has arrived
        {
            Mat outmat;
            if (disable_opengl && !bgmat.empty()) // merge in CPU to outmat if not using opengl
                outmat = bgmat.clone();
            // now begin to render
            if (!disable_opengl)
                gl_reset_screen();
            if (prodid > 0 && product_id != prodid)
            {
                int old_product_id = product_id;
                if (product_id > 0) // switch product
                {
                    LOG_INFO("Main rendering thread switch product from %d to %d", product_id, prodid);
                }
                product_id = prodid;

                for (auto &m : mlist)
                {
                    if (m.type == material::MT_Audio || m.type == material::MT_Video)
                    {
                        if (m.product_id > 0 && m.product_id == old_product_id) // non-current product, stop
                            stop_video_decoder_thread(m.ctx.reader, AV_STREAMING, check_is_stream(m)? stream_buffer_size : 0);
                        else if (m.product_id > 0 && m.product_id == product_id) // new current product, start
                            start_video_decoder_thread(m.ctx.reader, AV_STREAMING, check_is_stream(m)? stream_buffer_size : 0);
                    }
                }
            }
            if (has_stream_io && !first_frame_ready)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                now = std::chrono::steady_clock::now();
                wish = now;
                LOG_INFO("First frame ready, start to publish stream, num = %d", num);
            }
            first_frame_ready = true;

            // draw background
            if (!disable_opengl && !bgmat.empty())
            {
                AUTOTIMED("Render background run", first_run);
                if (output_alpha)
                    bgTexture = gl_render_texture_bgra(bgTexture, bgTexture? NULL : bgmat.data, bgmat.cols, bgmat.rows, 
                                    0, 0, output_width, output_height, 0, 0);
                else
                    bgTexture = gl_render_texture_bgr(bgTexture, bgTexture? NULL : bgmat.data, bgmat.cols, bgmat.rows, 
                                    0, 0, output_width, output_height, 0, 0);
            }

            {
            AUTOTIMED("Render mlist run", first_run);
            
            for(int i=static_idx; i<mlist.size(); i++)
            {
                auto &m = mlist[i];

                if (m.product_id > 0 && product_id != m.product_id)
                    continue;
                if(m.type == material::MT_MainAudio || m.type == material::MT_Audio) // skip audio
                    continue;

                if(m.type == material::MT_MainVideo)
                {
                    if((!disable_opengl) && alpha_video && strncasecmp(alpha_engine, "opengl", 7)==0)
                    {
                        mainvideo.ctx.glTexture = gl_render_texture_alpha(mainvideo.ctx.glTexture, frame.data, frame.channels(), frame.cols, frame.rows, 
                                    mainvideo.rect.x, mainvideo.rect.y, mainvideo.rect.width, mainvideo.rect.height, mainvideo.rotation, mainvideo.opacity, alpha_video[0]);
                    }
                    else
                    {
                        if (enable_chromakeying || disable_opengl)
                        {
                            if (enable_chromakeying)
                            {
                                AUTOTIMED("Remove background run", (enable_debug || first_run));
                                removeBackground(frame, frame, mask);
                                mainvideo.ctx.ftype = materialcontext::FT_BGRM;
                            }
                            OverlapImage(mainvideo, outmat, output_width, output_height, frame, mask, output_alpha);
                        }
                        else
                        {
                            // always redraw mainvideo
                            AUTOTIMED("Render mainvideo run", (enable_debug || first_run));
                            mainvideo.ctx.glTexture = gl_render_texture(mainvideo.ctx.glTexture, frame.data, frame.channels(), frame.cols, frame.rows, 
                                    mainvideo.rect.x, mainvideo.rect.y, mainvideo.rect.width, mainvideo.rect.height, mainvideo.rotation, mainvideo.opacity);
                        }
                    }
                    continue;
                }

                cv::Mat *pmat;
                {
                    AUTOTIMED("read_next_frame run", (enable_debug || first_run));
                    pmat = read_next_frame(m, ts, disable_opengl);
                    if(!pmat || pmat->empty())
                    {
                        continue;
                    }
                }

                {
                    AUTOTIMED("Render frame run", (enable_debug || first_run));
                    if (disable_opengl)
                    {
                        OverlapImage(m, outmat, output_width, output_height, *pmat, mask, output_alpha);
                    }
                    else
                    {
                        // always redraw gif and video, but not others
                        bool redraw = m.ctx.glTexture==0 || m.type==material::MT_Gif || m.type==material::MT_Video || m.type==material::MT_Clock;
                        if (m.ctx.ftype == materialcontext::FT_BGR)
                            m.ctx.glTexture = gl_render_texture_bgr(m.ctx.glTexture, redraw? pmat->data : NULL, pmat->cols, pmat->rows, 
                                        m.rect.x, m.rect.y, m.rect.width, m.rect.height, m.rotation, m.opacity);
                        else
                            m.ctx.glTexture = gl_render_texture_bgra(m.ctx.glTexture, redraw? pmat->data : NULL, pmat->cols, pmat->rows, 
                                        m.rect.x, m.rect.y, m.rect.width, m.rect.height, m.rotation, m.opacity);
                    }
                }
            }
            }

            if (water.text)
            {
                AUTOTIMED("Render water run", (enable_debug || first_run));
                if (disable_opengl)
                {
                    LOG_ERROR("Warning: does not support water text when opengl is disabled");
                }
                else
                {
                    bool repeat = (water.rows != 1 && water.cols != 1);
                    if (repeat)
                        waterTexture = gl_render_texture_mark(waterTexture, waterTexture? NULL : watermat.data,
                                    watermat.cols, watermat.rows, water.rotation, 0, MARK_REPEAT, water.rows, water.cols);
                    else
                    {
                        if (water.w == 0)
                            water.w = output_width;
                        if (water.h == 0)
                            water.h = output_height;
                        waterTexture = gl_render_texture_bgra(waterTexture, waterTexture? NULL : watermat.data,
                                    watermat.cols, watermat.rows, water.x, water.y, water.w, water.h, water.rotation, 100);
                    }
                }
            }

            if (subtitle)
            {
                AUTOTIMED("Render subtitle run", (enable_debug || first_run));
                int frameno = 0, retval;
                std::vector<unsigned char> subMat;
                static std::vector<unsigned char> oldSubMat;
                retval = ffSubtitleEncodeThread.Read(subMat, frameno, num, fps);
                if (retval < 0)
                {
                    LOG_INFO("Warning: read subtitle error, use previous one");
                    if (disable_opengl)
                        subMat = oldSubMat;
                }
                else if (disable_opengl)
                {
                    oldSubMat = subMat;
                }
                if (subMat.size() || subTexture)
                {
                    if (disable_opengl)
                    {
                        material m;
                        m.ctx.ftype = materialcontext::FT_BGRA;
                        m.rect = {ffSubtitleEncodeThread.sub_x, ffSubtitleEncodeThread.sub_y, ffSubtitleEncodeThread.sub_w, ffSubtitleEncodeThread.sub_h};
                        cv::Mat mask, mat(cv::Size(ffSubtitleEncodeThread.sub_w, ffSubtitleEncodeThread.sub_h), CV_8UC4, subMat.data());
                        OverlapImage(m, outmat, output_width, output_height, mat, mask, output_alpha);
                    }
                    else
                    {
                        subTexture = gl_render_texture_bgra(subTexture, subMat.empty()? NULL : subMat.data(),
                                    ffSubtitleEncodeThread.sub_w, ffSubtitleEncodeThread.sub_h,
                                    ffSubtitleEncodeThread.sub_x, ffSubtitleEncodeThread.sub_y,
                                    ffSubtitleEncodeThread.sub_w, ffSubtitleEncodeThread.sub_h, 0, 100);
                    }
                }
                else
                {
                    LOG_INFO("Warning: read subtitle error and no previous subtitle available, skip");
                }
            }

            {
                AUTOTIMED("Dowload image run", (enable_debug || first_run));
                if (!disable_opengl)
                    gl_download_image(base.data);
                else if (!outmat.empty())
                        base = outmat;
            }

            if (blind_watermark) {
                blind_watermark->setFps(fps);
                blind_watermark->draw(base);
            }

            // write substream out if needed
            if (substream_out.video_thread && !substream_out.video_thread->bStopped)
            {
                AUTOTIMED("Write substream run", (enable_debug || first_run));
                bool skip = false;
                if (substream_out.cur_frameno > 0)
                {
                    double main_timebase = ((double)(num-substream_out.start_frameno)) / fps;
                    double sub_timebase = ((double)substream_out.cur_frameno + 0.5) / substream_out.videoinfo.fps;
                    if (sub_timebase >= main_timebase)
                    {
                        skip = true;
                    }
                }
                if (!skip)
                {
                    cv::Mat subMat;
                    cv::resize(base, subMat, cv::Size(substream_out.videoinfo.w, substream_out.videoinfo.h), 0, 0, cv::INTER_NEAREST);
                    substream_out.video_thread->Write(subMat.data, subMat.rows*subMat.cols*subMat.channels(), EC_RAWMEDIA_RAWVIDEO);
                    substream_out.cur_frameno ++;
                }
            }

            if (!output_alpha && !rawdata_out)
            {
                AUTOTIMED("EncoderThread::Write1 run", (enable_debug || first_run));
                { // from experience, opencv conversion to yuv420p is a little faster (about 10%) than ffmpeg/sws_scale
                    AUTOTIMED("CV::Convert YUV run", (enable_debug || first_run));
                    cv::cvtColor(base, yuv, COLOR_BGR2YUV_I420);
                }
                ffVideoEncodeThread.Write(yuv.data, yuv.rows*yuv.cols, EC_RAWMEDIA_RAWVIDEO);
            }
            else // write yuv420p/bgra data to ffmpeg fifo
            {
                AUTOTIMED("EncoderThread::Write2 run", (enable_debug || first_run));
                bool keep_alpha = output_alpha;
                cv::Mat newMat;
                if (strncasecmp(output_fmt, "mp4alpha", 9)==0)
                {
                    AUTOTIMED("Convert mp4alpha frame run", (enable_debug || first_run));
                    // extract alpha to gray image
                    cv::Mat grayMat(base.size(), CV_8UC3);
                    int from_to[] = {3, 0, 3, 1, 3, 2};
                    cv::mixChannels(&base, 1, &grayMat, 1, from_to, 3);
                    cv::Mat bgrMat;
                    cv::cvtColor(base, bgrMat, cv::COLOR_BGRA2BGR);

                    if (output_width > output_height) // add alpha to bottom-side
                    {
                        newMat.create(cv::Size(base.cols, base.rows*2), CV_8UC3);
                        bgrMat.copyTo(newMat(cv::Rect(0, 0, base.cols, base.rows)));
                        grayMat.copyTo(newMat(cv::Rect(0, base.rows, base.cols, base.rows)));
                    }
                    else // add alpha to right-side
                    {
                        newMat.create(cv::Size(base.cols*2, base.rows), CV_8UC3);
                        bgrMat.copyTo(newMat(cv::Rect(0, 0, base.cols, base.rows)));
                        grayMat.copyTo(newMat(cv::Rect(base.cols, 0, base.cols, base.rows)));
                    }
                    keep_alpha = false;
                }
                else
                {
                    newMat = base;
                }
                int channels = keep_alpha? 4:3;
                ffVideoEncodeThread.Write(newMat.data, newMat.rows*newMat.cols*channels,
                                rawdata_out? EC_RAWMEDIA_VIDEO:EC_RAWMEDIA_RAWVIDEO);
            }
        } // if (!frame.empty())
        else
        {
            LOG_INFO("Warning: main video frame is not available, num=%d", num);
        }

        // mix and encode audio
        std::vector<pcm_info> all_audios;
        pcm_info main_audio_pcm = { .length = 0, .pcm = NULL, .volum = 0 };
        std::vector<unsigned char> audiobuf;
        FFReader *reader = NULL;
        ret = 0;
        // if mainaudio and mainvideo both have audio, then use mainaudio and discard mainvideo's audio
        if (mainaudio.type == material::MT_MainAudio && !rawaudio.aud_fmt)
        {
            main_adata.clear();
            reader = mainaudio.ctx.reader;
            AUTOTIMED(("Read audio (size:"+std::to_string(audio_bytes_cur_frame)+") run").c_str(), (enable_debug || first_run));
            if (audio_bytes_cur_frame > 1 && (ret=read_audio_data(reader, audiobuf, audio_bytes_cur_frame)) >= 0 && audiobuf.size() > 1)
            {
                main_adata = audiobuf;
                main_audio_pcm.length = audiobuf.size() / 2;
                main_audio_pcm.pcm = (short *)audiobuf.data();
                main_audio_pcm.volum = mainvideo.type==material::MT_MainVideo? mainvideo.volume : (mainaudio.type==material::MT_MainAudio? mainaudio.volume : 100);
                if (main_audio_pcm.volum <= 0 || main_audio_pcm.volum > 100)
                    main_audio_pcm.volum = 100;
                all_audios.push_back(main_audio_pcm);
            }
            else if (ret == 0 && reader->decode_audio) // has sound, but may not be ready
            {
                LOG_INFO("Warning: main audio is not ready");
            }
        }
        else if(main_adata.size()) // use mainvideo's audio
        {
            main_audio_pcm.length = main_adata.size() / 2;
            main_audio_pcm.pcm = (short *)main_adata.data();
            main_audio_pcm.volum = mainvideo.type==material::MT_MainVideo? mainvideo.volume : (mainaudio.type==material::MT_MainAudio? mainaudio.volume : 100);
            if (main_audio_pcm.volum <= 0 || main_audio_pcm.volum > 100)
                main_audio_pcm.volum = 100;
            all_audios.push_back(main_audio_pcm);
        }
        else if((main_vdata.size() && ffreader->decode_audio) || // has main video but audio has not arrived
                audio_bytes_cur_frame < 2) // already sent too much
        {
            if (main_vdata.size() && ffreader->decode_audio && has_stream_io)
            {
                LOG_INFO("Warning: main video's audio has not arrived.");
            }
            main_audio_pcm.pcm = (short *)&audiobuf; // stub address
            main_audio_pcm.length = 0;
        }

        // if main audio is not ready, skip audio output
        if (main_audio_pcm.pcm==NULL || // no main audio
             main_audio_pcm.length)     // main audio is ready
        {
            std::vector<unsigned char> mixed_audio;
            {
                AUTOTIMED("Mix audio run", (enable_debug || first_run));
                vector<unsigned char> audio_bufs[mlist.size()];
                for(int i=0; i<mlist.size(); i++)
                {
                    auto &m = mlist[i];
                    if (m.product_id > 0 && product_id != m.product_id)
                        continue;
                    if(m.type != material::MT_Audio && m.type != material::MT_Video) // skip non audio
                        continue;
                    if(m.volume <= 0 || m.ctx.reader==NULL) // open failed, should not happen, skip
                        continue;

                    int ret = read_next_audio(m, audio_bufs[i], main_audio_pcm.pcm? main_audio_pcm.length*2 : audio_bytes_cur_frame);
                    if (ret >= 0 && audio_bufs[i].size() > 1)
                    {
                        pcm_info pcm;
                        pcm.length = audio_bufs[i].size() / 2;
                        pcm.pcm = (short *)audio_bufs[i].data();
                        pcm.volum = m.volume;
                        all_audios.push_back(pcm);
                    }
                }

                // if no audios, merge_audio will output a dummy audio
                mverge_audio(all_audios, main_audio_pcm.pcm? main_audio_pcm.length : audio_bytes_cur_frame/2, mixed_audio);
            }

            sent_audio_bytes += mixed_audio.size();
            {
                AUTOTIMED("Write audio run", (enable_debug || first_run));
#if 0
                // play with: ffplay -f s16le -ar 16000 -ac 1 mixed.pcm
                static FILE *pcm_file = NULL;
                if (pcm_file == NULL)
                {
                    pcm_file = fopen("mixed.pcm", "w");
                }
                fwrite(mixed_audio.data(), 1, mixed_audio.size(), pcm_file);
#endif
                if (substream_out.audio_thread && !substream_out.audio_thread->bStopped && !substream_out.disable_audio)
                {
                    substream_out.audio_thread->Write(mixed_audio.data(), mixed_audio.size());
                }
                if(rawdata_out)
                    ffVideoEncodeThread.Write(mixed_audio.data(), mixed_audio.size(), EC_RAWMEDIA_AUDIO);
                else
                    ffAudioEncodeThread.Write(mixed_audio.data(), mixed_audio.size());
            }
        }

        // display
        if(enable_window && !frame.empty())
        {
            imshow("Decorated Video", base);

            // press ESC to exit
            if(waitKey(25) == 27)
                break;
        }

        if (mainvideo.type == material::MT_MainVideo && frame.empty() ||
            (mainvideo.type != material::MT_MainVideo && mainaudio.type == material::MT_MainAudio && main_adata.empty() && !mainaudio_missing))
        {
            LOG_INFO("Main %s EOF.", mainvideo.type == material::MT_MainVideo? "video" : "audio");
            break;
        }

        if (has_stream_io)
        {
            wish += timebase;
            now = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::nanoseconds>(wish - now);
            if (diff >= dns)
            {
                std::this_thread::sleep_for(diff);
            }
            else if (diff < max_dns)
            {
                LOG_ERROR("process too long for video frame, diff = %lldms, num = %d", diff.count()/1000000, num);
            }
        }
        first_run = (num < 3);

        ts += 1000.0 / fps; // miliseconds elapsed
    }

    LOG_INFO("Finished decoration process, total frames %d, total time %fms.", num, ts);
    if (mainvideo.type == material::MT_MainVideo ||
       (mainaudio.type == material::MT_MainAudio && rawaudio.aud_fmt))
        close_video_thread(ffreader);
    else if (ffreader)
        read_video_close(ffreader);

    ffAudioEncodeThread.EXIT(false);
    ffVideoEncodeThread.EXIT(false);
    if (writer_pipe)
    {
        if (rawdata_out)
            fclose(writer_pipe);
        else
            pclose(writer_pipe);
    }
    close_streamout_thread(&substream_out, true);

    if(enable_window)
    {
        destroyAllWindows();
    }

    }

    gl_uninit_render();
    remove(out_audio_fifoname.c_str());
    if (substream_out.audio_fifo[0])
        remove(substream_out.audio_fifo);

    fflush(stdout);
    fflush(stderr);
    // exit without executing at_exit functions
    _exit(0);
    return 0;
}

