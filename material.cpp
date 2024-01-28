#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <atomic>
#include "material.h"
#include "matops.h"
#include "decorateVideo.h"
#include "3rd/gifdecoder/ffgif.h"
#include "event.h"
#include "3rd/log/LOGHelp.h"

#undef	__MODULE__
#define __MODULE__ "Material"

using namespace std;
using namespace cv;

material::MaterialType string2type(const string &s)
{
    if(strncasecmp(s.c_str(), "video", s.length()) == 0)
        return material::MT_Video;
    if(strncasecmp(s.c_str(), "mainvideo", s.length()) == 0)
        return material::MT_MainVideo;
    if(strncasecmp(s.c_str(), "gif", s.length()) == 0)
        return material::MT_Gif;
    if(strncasecmp(s.c_str(), "image", s.length()) == 0)
        return material::MT_Image;
    if(strncasecmp(s.c_str(), "mainaudio", s.length()) == 0)
        return material::MT_MainAudio;
    if(strncasecmp(s.c_str(), "audio", s.length()) == 0)
        return material::MT_Audio;
    if(strncasecmp(s.c_str(), "text", s.length()) == 0)
        return material::MT_Text;
    if(strncasecmp(s.c_str(), "time", s.length()) == 0)
        return material::MT_Time;
    if(strncasecmp(s.c_str(), "clock", s.length()) == 0)
        return material::MT_Clock;
    return material::MT_None;
}

std::vector<std::string> splitString(const std::string &str, char sep)
{
    std::vector<std::string> ss;
    size_t pos = 0;
    while(pos != str.npos)
    {
        auto next_pos = str.find(sep, pos);
        std::string sub;
        if(next_pos == str.npos)
        {
            sub = str.substr(pos);
            pos = str.npos;
        }
        else
        {
            sub = str.substr(pos, next_pos - pos);
            pos = (next_pos+1)==str.length()? str.npos : (next_pos + 1);
        }
        ss.push_back(sub);
    }
    return ss;
}

std::vector<char *> splitCString(char *str, char sep)
{
    std::vector<char *> ss;
    char *pos = str;
    while(pos)
    {
        char *next_pos = strchr(pos, sep);
        char *sub = pos;
        if(next_pos == NULL)
            pos = NULL;
        else
        {
            *next_pos = 0;
            pos = next_pos + 1;
        }
        ss.push_back(sub);
    }
    return ss;
}

bool file_exists(const std::string &file)
{
    int fd = open(file.c_str(), O_RDONLY);
    close(fd);
    return fd > 0;
}

void merge_path(char *out, int out_size, const char *dir, const char *path)
{
    if(path[0]=='/' || strncmp(path, "-", 2)==0 || strstr(path, "://") != NULL || dir == NULL || dir[0] == 0)
    {
        strncpy(out, path, out_size-1);
        out[out_size-1] = 0;
    }
    else
    {
        if(path[0]=='.' && path[1]=='/')
            path += 2;
        snprintf(out, out_size-1, "%s/%s", dir, path);
        out[out_size-1] = 0;
    }
}

void merge_pathlist(char *out, int out_size, const char *dir, const char *pathlist)
{
    std::vector<std::string> strs = splitString(pathlist, ',');
    
    for (int i=0; i<strs.size(); i++)
    {
        auto &s = strs[i];
        char out2[1024];
        merge_path(out2, sizeof(out2), dir, s.c_str());
        auto len = strlen(out2);
        if (out_size < len + 2)
        {
            fprintf(stderr, "Not enough size for merging pathlist, dir=%s, pathlist=%s.\n", dir, pathlist);
            return;
        }
        if (i)
        {
            *out++ = ',';
            out_size --;
        }
        memcpy(out, out2, len);
        out[len] = 0;
        out += len;
        out_size -= len;
    }
}

int ensure_dir_exists(const char *path)
{
    string dir = path;
    auto slash = dir.find_last_of('/');
    if (slash != dir.npos)
    {
        // the directory may not exist, creat it.
        dir = dir.substr(0, slash);
        int ret = system(("mkdir -p " + dir).c_str());
        if(ret == -1)
        {
            LOG_ERROR("Failed to create output directory %s", dir.c_str());
            return -1;
        }
    }
    return 0;
}

int parse_color(const char *colorstr, vector<unsigned char> &bgr)
{
    bgr.resize(3);
    if(strlen(colorstr)==3) // 12-34-56 short 11-33-55 to 1-3-5
    {
        char r[] = {colorstr[0], colorstr[0], 0};
        char g[] = {colorstr[1], colorstr[1], 0};
        char b[] = {colorstr[2], colorstr[2], 0};
        bgr[2] = strtol(r, 0, 16);
        bgr[1] = strtol(g, 0, 16);
        bgr[0] = strtol(b, 0, 16);
    }
    else
    {
        int val = strtol(colorstr, 0, 16);
        bgr[2] = (val >> 16) & 0xff; // r
        bgr[1] = (val >> 8) & 0xff;  // g
        bgr[0] = val & 0xff;         // b
    }
    return 0;
}

static int parse_material_text(std::vector<std::string> &strs, material &m, const char *data_dir, double x_ratio, double y_ratio)
{
    // <type>:<layer>:<text>:<top>:<left>:<width>:<height>:<fontsize>:<rotation>:<opacity>:<font>:<color>:<outlinesize>:<outlinecolor>

    m.color[3] = m.olcolor[3] = 0;
    for(int idx = 0; idx < strs.size(); idx ++)
    {
        std::string &s1 = strs[idx];
        switch(idx)
        {
            case 0: // type, already handled
                break;
            case 1: // layer
                m.layer = atoi(s1.c_str());
                if (m.layer<=0)
                {
                    LOG_ERROR("Invalid %s layer argument, must start at 1", m.type==material::MT_Text? "text" : "time");
                    return -1;
                }
                break;
            case 2: // text
                strncpy(m.text, s1.c_str(), sizeof(m.text) - 1);
                m.text[sizeof(m.text) - 1] = 0;
                break;
            case 3: // top
                m.rect.y = atoi(s1.c_str()) * y_ratio;
                break;
            case 4: // left
                m.rect.x = atoi(s1.c_str()) * x_ratio;
                break;
            case 5: // width
                m.rect.width = atoi(s1.c_str()) * x_ratio;
                break;
            case 6: // height
                m.rect.height = atoi(s1.c_str())* y_ratio;
                break;
            case 7: // font size
                m.fontsize = atoi(s1.c_str());
                break;
            case 8: // rotation
                m.rotation = atoi(s1.c_str()) % 360;
                break;
            case 9: // opacity
                m.opacity = atoi(s1.c_str());
                if (m.opacity <= 0 || m.opacity > 100) // 0 is equal to 100
                    m.opacity = 100;
                break;
            case 10: // font
                strncpy(m.font, s1.c_str(), sizeof(m.font) - 1);
                m.font[sizeof(m.font) - 1] = 0;
                // replace %20% with space
                {
                    char *pspace = strstr(m.font, "%20%");
                    while(pspace)
                    {
                        *pspace++ = ' ';
                        memmove(pspace, pspace+3, strlen(pspace+3)+1);
                        pspace = strstr(pspace, "%20%");
                    }
                }
                break;
            case 11: // font color
                {
                    if (s1[0] != '#' || s1.length() < 4)
                    {
                        LOG_ERROR("Invalid %s font color format, must be #RRBBGG format", m.type==material::MT_Text? "text" : "time");
                        return -1;
                    }
                    vector<unsigned char> bgr;
                    parse_color(s1.c_str()+1, bgr);
                    m.color[0] = bgr[0];
                    m.color[1] = bgr[1];
                    m.color[2] = bgr[2];
                    m.color[3] = 0xff;
                }
                break;
            case 12: // outline size
                m.olsize = atoi(s1.c_str());
                break;
            case 13: // outline color
                {
                    if (s1[0] != '#' || s1.length() < 4)
                    {
                        LOG_ERROR("Invalid %s outline color format, must be #RRBBGG format", m.type==material::MT_Text? "text" : "time");
                        return -1;
                    }
                    vector<unsigned char> bgr;
                    parse_color(s1.c_str()+1, bgr);
                    m.olcolor[0] = bgr[0];
                    m.olcolor[1] = bgr[1];
                    m.olcolor[2] = bgr[2];
                    m.olcolor[3] = 0xff;
                }
                break;
            default: 
                break;
        }
    }
    // valid checking
    if((m.rect.width && m.rect.height==0) ||
            (m.rect.width==0 && m.rect.height))
    {
        LOG_ERROR("Invalid material width/height values for %s: %s", m.type==material::MT_Text? "text" : "time", m.text);
        return -1;
    }
    LOG_INFO("Got one %s material: { layer:%d, %s:%s, rect:{%d,%d,%d,%d}, font:%s, fontsize:%d }", m.type==material::MT_Text? "text" : "time",
            m.layer, m.type==material::MT_Text? "text" : "time", m.text, m.rect.x, m.rect.y, m.rect.width, m.rect.height, m.font, m.fontsize);
    return 0;
}

bool check_is_stream(const char *path)
{
    return strncmp(path, "rtmp://", 7)==0;
}

bool check_is_stream(material &m)
{
    return ((m.type==material::MT_Video || m.type==material::MT_Audio || 
              m.type==material::MT_MainVideo) && check_is_stream(m.path));
}

bool check_is_audio(material &m)
{
    return m.type==material::MT_Audio || m.type==material::MT_MainAudio;
}

bool check_has_stream(std::vector<material> &mlist)
{
    for (auto &m : mlist)
    {
        if (check_is_stream(m))
            return true;
    }
    return false;
}

int parse_material(const std::string &s, material &mm, const char *data_dir, double x_ratio, double y_ratio)
{
    material m = { .layer = 0, .type = material::MT_None, .product_id = 0, .material_id = 0, .text = {0}, .path = {0}, .rect = {0,0,0,0}, .ori_video_rect = {0,0,0,0}, .volume = 0, .rotation=0, .opacity=100, .font = {0}, .fontsize = 0, .olsize = 0, .color = {0}, .olcolor = {0}, .ctx = {0} };

    std::vector<std::string> strs;
    const char *p = NULL;
    if (strncmp(s.c_str(), "product:", 8)==0)
    {
        m.product_id = atoi(s.c_str()+8);
        p = strchr(s.c_str()+8, ':');
        if (p)
        {
            m.material_id = atoi(p+1);
            p = strchr(p+1, ':');
        }
        if (p == NULL)
        {
            LOG_ERROR("Invalid product argument: %s", s.c_str());
            return -1;
        }
        p ++;
    }
    strs = splitString(p? p : s.c_str(), ':');

    if (strs.size() >= 4) // strs[2] == path, may be rtmp://xxx or shm://xxx
    {
        if ((strs[2] == "rtmp" || strs[2] == "http" || strs[2] == "https" || strs[2] == "shm" )
            && strs[3][0] == '/' && strs[3][1] == '/')
        {
            strs[2] += ":" + strs[3];
            strs.erase(strs.begin()+3);
            // http/rtmp may contain port like: rtmp://server:port/appid/streamid
            if (strs.size() >= 4 && strs[3].find('/')!=strs[3].npos)
            {
                strs[2] += ":" + strs[3];
                strs.erase(strs.begin()+3);
            }
        }
    }

    for(int idx = 0; idx < strs.size(); idx ++)
    {
        std::string &s1 = strs[idx];
        switch(idx)
        {
            case 0: // type
                m.type = string2type(s1);
                if (m.type == material::MT_None)
                {
                    LOG_ERROR("Invalid type argument in marterial: %s", s.c_str());
                    return -1;
                }
                if (m.type == material::MT_Text || m.type == material::MT_Time)
                {
                    int ret = parse_material_text(strs, m, data_dir, x_ratio, y_ratio);
                    if (ret == 0)
                    {
                        mm = m;
                    }
                    return ret;
                }
                break;
            case 1: // layer
                m.layer = atoi(s1.c_str());
                if (m.type != material::MT_Audio && m.type != material::MT_MainAudio && m.layer<=0)
                {
                    LOG_ERROR("Invalid layer argument, must start at 1, marterial: %s", s.c_str());
                    return -1;
                }
                break;
            case 2: // path
                if (m.type == material::MT_Clock)
                    merge_pathlist(m.path, sizeof(m.path), data_dir, s1.c_str());
                else
                    merge_path(m.path, sizeof(m.path), data_dir, s1.c_str());
                 break;
            case 3: // top
                m.rect.y = atoi(s1.c_str()) * y_ratio;
                break;
            case 4: // left
                m.rect.x = atoi(s1.c_str()) * x_ratio;
                break;
            case 5: // width
                m.rect.width = atoi(s1.c_str()) * x_ratio;
                break;
            case 6: // height
                m.rect.height = atoi(s1.c_str())* y_ratio;
                break;
            case 7: // audio volume
                if(m.type==material::MT_MainVideo || m.type==material::MT_MainAudio ||
                        m.type==material::MT_Video || m.type==material::MT_Audio)
                {
                    m.volume = atoi(s1.c_str());
                    if (m.volume <= 0)
                        m.volume = 0;
                    if (m.volume >= 100)
                        m.volume = 100;
                }
                break;
            case 8: // rotation
                m.rotation = atoi(s1.c_str()) % 360;
                break;
            case 9: // opacity
                m.opacity = atoi(s1.c_str());
                if (m.opacity <= 0 || m.opacity > 100) // 0 is equal to 100
                    m.opacity = 100;
                break;
            default: 
                break;
        }
    }
    // valid checking
    if((m.type!=material::MT_Audio && m.type!=material::MT_MainAudio && m.layer <=0) ||
         strlen(m.path) == 0 || m.type == material::MT_None)
    {
        LOG_ERROR("Invalid material arguments: %s", s.c_str());
        return -1;
    }
    if((m.rect.width && m.rect.height==0) ||
            (m.rect.width==0 && m.rect.height))
    {
        LOG_ERROR("Invalid material width/height values in %s", s.c_str());
        return -1;
    }
    if((m.type==material::MT_MainAudio || m.type==material::MT_MainVideo || m.type==material::MT_Audio) && m.volume == 0)
    {
        m.volume = 100;
    }
    LOG_INFO("Got one material: { type:%d, layer:%d, path:%s, rect:{%d,%d,%d,%d} }",
            m.type, m.layer, m.path, m.rect.x, m.rect.y, m.rect.width, m.rect.height);
    mm = m;
    return 0;
}

int parse_materials(int argc, char **argv, material &mainvideo, material &mainaudio, std::vector<material> &mlist, const char *data_dir, double x_ratio, double y_ratio)
{
    mainvideo.type = material::MT_None;
    mainaudio.type = material::MT_None;
    mlist.clear();

    for(int i = 2; i < argc; i++)
    {
        material m;
        if(parse_material(argv[i], m, data_dir, x_ratio, y_ratio))
        {
            return -1;
        }
        if(m.type == material::MT_MainVideo || m.type == material::MT_MainAudio)
        {
            if ((m.type == material::MT_MainVideo && mainvideo.type == material::MT_MainVideo)
             || (m.type == material::MT_MainAudio && mainvideo.type == material::MT_MainAudio))
            {
                LOG_ERROR("There must be only one main video/audio.");
                return -1;
            }
            if(m.type == material::MT_MainVideo)
                mainvideo = m;
            if(m.type == material::MT_MainAudio)
                mainaudio = m;
        }
        mlist.push_back(m);
    }
    std::sort(mlist.begin(), mlist.end(), 
        [](const material &a2, const material &a1)->bool{
            return (a2.layer - a1.layer) < 0;
        });

    LOG_INFO("Sorted materials:");
    for(int i=0; i<mlist.size(); i++)
    {
        auto &m = mlist[i];
        printf("material %-2d: { type:%d, layer:%d, path:%s, rect:{%d,%d,%d,%d} }\n",
             i, m.type, m.layer, m.path, m.rect.x, m.rect.y, m.rect.width, m.rect.height);
    }
    return 0;
}

void close_material(material &m)
{
    switch(m.type)
    {
    case material::MT_MainAudio:
        break;
    case material::MT_Audio:
    case material::MT_Video:
        // open in thread
        close_video_thread(m.ctx.reader, true);
        m.ctx.reader = NULL;
        break;
    case material::MT_Text:
    case material::MT_Time:
        break;
    case material::MT_Image:
    case material::MT_Clock:
    case material::MT_Gif:
        break;
    default: // including main video
        break;
    }
}

void get_text_image(material &m, const char *text)
{
    int water_width = m.rect.width;
    int water_height = m.rect.height;
    cv::Point startPos(0, 10);
    if (m.opacity>0 && m.opacity < 100 && m.type == material::MT_Time)
    {
        m.ctx.time_opacity = m.opacity;
        m.opacity = 100;
    }
    if ((m.rotation % 360) != 0 && m.type == material::MT_Time)
    {
        m.ctx.time_rotation = m.rotation;
        m.rotation = 0;
    }
    int opacity = m.type == material::MT_Time? m.ctx.time_opacity : m.opacity;
    int rotation = m.type == material::MT_Time? m.ctx.time_rotation : m.rotation;
    if (opacity <=0 || opacity > 100)
        opacity = 100;

    auto mat = text2Mat(text, m.font, m.fontsize * 2,
            cv::Scalar(m.color[0], m.color[1], m.color[2], ((double)opacity)*255/100),
            startPos, water_width, water_height, cvx::WRAP_ALIGN_CENTER, 
            m.olcolor[3]? cv::Scalar(m.olcolor[0], m.olcolor[1], m.olcolor[2], ((double)opacity)*255/100) : cv::Scalar(0,0,0,0));
    if (mat.empty()) // probably failed to load font
    {
        LOG_ERROR("Failed to draw text %s on rect [%d,%d,%d,%d]", text, m.rect.x, m.rect.y, m.rect.width, m.rect.height);
        mat = cv::Mat(cv::Size(water_width, water_height), CV_8UC4, cv::Scalar(0.0));
        cv::putText(mat, text, startPos, cv::FONT_HERSHEY_SIMPLEX, m.fontsize / 10.0, cv::Scalar(m.color[0], m.color[1], m.color[2], ((double)opacity)*255/100));
    }
    else
    {
        int x = m.rect.width > mat.cols? (m.rect.width - mat.cols) / 2 : 0;
        int y = m.rect.height > mat.rows? (m.rect.height - mat.rows) / 2 : 0;
        int w = m.rect.width > mat.cols? mat.cols : m.rect.width;
        int h = m.rect.height > mat.rows? mat.rows : m.rect.height;
        cv::Mat mat2(cv::Size(m.rect.width, m.rect.height), CV_8UC4, cv::Scalar::all(0));
        mat.copyTo(mat2(cv::Rect(x, y, w, h)));
        mat = mat2;
    }
    m.ctx.ftype = materialcontext::FT_BGRA;
    if (rotation % 360)
    {
        cv::Point pos(m.rect.x, m.rect.y);
        mat = RotateMat(mat, pos, rotation % 360);
        m.rect = cv::Rect(pos.x, pos.y, mat.cols, mat.rows);
        m.ctx.ftype = materialcontext::FT_BGRA;
    }
    m.opacity = 100;
    m.rotation = 0;
    m.ctx.frames.push_back(mat.clone());
    m.ctx.w = mat.cols;
    m.ctx.h = mat.rows;
    m.ctx.fps = 0;
}

int open_materials(std::vector<material> &mlist, double x_ratio, double y_ratio, rawaudioinfo *audioinfo, material *mainaudio, int stream_buffer_size, int out_fps, bool disable_opengl, int product_id)
{
    for(auto &m : mlist)
    {
        cv::Mat mat;
        int ret;

        switch(m.type)
        {
        case material::MT_Gif:
            {
                ret = decode_gif(m.path, m.ctx.frames, m.ctx.fps_times);
                if (ret || m.ctx.frames.size() <= 0)
                {
                    LOG_ERROR("Error opening gif file %s", m.path);
                    return -1;
                }
__decode_gif:
                // change absolute time to relative time, and change unit from second to ms
                double timebase = (m.ctx.fps_times[m.ctx.fps_times.size()-1] / (m.ctx.frames.size()-1)) * 1000;
                auto oldRect = m.rect;
                for(int i=m.ctx.frames.size()-1; i>=0; i--)
                {
                    if (i > 0)
                    {
                        m.ctx.fps_times[i] = (m.ctx.fps_times[i] - m.ctx.fps_times[i-1]) * 1000;
                    }
                    else
                    {
                        m.ctx.fps_times[i] = timebase;
                    }
                    auto &mm = m.ctx.frames[i];
                    if (m.rect.width == 0)
                        oldRect.width = m.rect.width = mm.cols * x_ratio;
                    if (m.rect.height == 0)
                        oldRect.height = m.rect.height = mm.rows * y_ratio;

                    if (disable_opengl || m.ctx.frames.size() < 100) // if too many frames, pass on to opengl
                    {
                        if (oldRect.width != mm.cols || oldRect.height != mm.rows)
                            cv::resize(mm, mm, cv::Size(oldRect.width, oldRect.height), 0.0, 0.0, cv::INTER_CUBIC);
                        if (m.opacity > 0 && m.opacity < 100)
                        {
                            float fop = ((float)m.opacity)/100;
                            // extract alpha image
                            int from_to[] = {3, 0};
                            cv::Mat gray_image(mm.size(), CV_8UC1);
                            mixChannels(&mm, 1, &gray_image, 1, from_to, 1);
                            // multiply by opactity
                            gray_image.convertTo(gray_image, CV_8UC1, fop);
                            // insert back
                            cv::insertChannel(gray_image, mm, 3);
                        }
                        if (m.rotation % 360)
                        {
                            cv::Point pos(oldRect.x, oldRect.y);
                            RotateMat(mm, pos, m.rotation % 360).copyTo(mm);
                            m.rect = cv::Rect(pos, mm.size());
                        }
                    }
                }
                m.ctx.w = m.ctx.frames[0].cols;
                m.ctx.h = m.ctx.frames[0].rows;
                m.ctx.fps = 1000.0/timebase;
                m.ctx.ftype = materialcontext::FT_BGRA;
                if (disable_opengl || m.ctx.frames.size() < 100)
                {
                    m.opacity = 100;
                    m.rotation = 0;
                }
                LOG_INFO("Gif file %s got %d frames with size %dx%d, fps %f", m.path, m.ctx.frames.size(), m.ctx.w, m.ctx.h, m.ctx.fps);
            }
            break;
        case material::MT_Audio:
        case material::MT_MainAudio:
        case material::MT_Video:
            if (m.type == material::MT_MainAudio && audioinfo->aud_fmt) // raw mainaudio, use threading read
                break;
            if (m.type == material::MT_MainAudio) // open in main thread
                m.ctx.reader = open_video_reader(m.path, m.type==material::MT_Video? true:false, m.volume>0? true:false,
                                             m.rect.width, m.rect.height, "speed", AV_PIX_FMT_BGR24,
                                             AV_SAMPLE_FMT_S16, audioinfo->channel, audioinfo->samplerate);
            else // open in thread
                m.ctx.reader = new_video_reader(m.path, m.type==material::MT_Video? true:false, m.volume>0? true:false,
                                             m.rect.width, m.rect.height, "speed", AV_PIX_FMT_BGR24,
                                             AV_SAMPLE_FMT_S16, audioinfo->channel, audioinfo->samplerate);
            if(m.ctx.reader == NULL)
            {
                if (check_is_stream(m))
                {
                    send_event(ET_STREAM_NONEXIST, "Stream not found");
                }
                LOG_ERROR("Error opening material file %s", m.path);
                return -1;
            }
            m.ctx.audindex = -1;
            m.ctx.bufindex = 0;
            m.ctx.frames.clear();
            m.ctx.audio.clear();
            if(m.type == material::MT_Video)
            {
                m.ctx.ftype = materialcontext::FT_BGR;
                m.ctx.fps = 0;
            }
            else
            {
                m.ctx.fps = out_fps;
            }
            if (m.type == material::MT_MainAudio)
            {
                mainaudio->ctx.reader = m.ctx.reader;
            }
            else
            {
                bool is_audio = check_is_audio(m);
                if (is_audio)
                    m.ctx.reader->fps = out_fps;
                if (m.product_id > 0 && product_id > 0 && m.product_id != product_id) // non-current product, stop
                    stop_video_decoder_thread(m.ctx.reader, AV_STREAMING, check_is_stream(m)? stream_buffer_size : 0);
                else
                    start_video_decoder_thread(m.ctx.reader, AV_STREAMING, check_is_stream(m)? stream_buffer_size : 0);
            }
            break;
    
        case material::MT_Text:
            {
                std::string fmt = m.text;
                replace_all(fmt, "%20%", " ");
                replace_all(fmt, "%c%", ":");
                get_text_image(m, fmt.c_str());
                LOG_INFO("text2image [%s] got one frame with size=[%d,%d]", m.text, m.ctx.w, m.ctx.h);
            }
            break;

        case material::MT_Image:
            mat = cv::imread(m.path, IMREAD_UNCHANGED); // without apha channel
            if(mat.empty())
            {
                ret = decode_gif(m.path, m.ctx.frames, m.ctx.fps_times);
                if (ret || m.ctx.frames.size() <= 0)
                {
                    LOG_ERROR("Error opening image file %s", m.path);
                    return -1;
                }
                m.type = material::MT_Gif;
                goto __decode_gif;
            }
            if(mat.channels() == 4)
            {
                // currently we only support 8bit color
                if(mat.type()!=CV_8UC4 && mat.type()!=CV_8UC3)
                {
                    mat.convertTo(mat, CV_8UC4, 1.0/255, 0);
                }
                if (mat.type()==CV_8UC3)
                {
                    LOG_ERROR("Warning: image has 4 channels but type is CV_8UC3");
                }
                m.ctx.ftype = materialcontext::FT_BGRA;
            }
            else
            {
                if(mat.type()!=CV_8UC3)
                {
                    mat.convertTo(mat, CV_8UC3, 1.0/255, 0);
                }
                m.ctx.ftype = materialcontext::FT_BGR;
            }
            if (m.rect.width == 0)
                m.rect.width = mat.cols * x_ratio;
            if (m.rect.height == 0)
                m.rect.height = mat.rows * y_ratio;
            if (m.rect.width != mat.cols || m.rect.height != mat.rows)
                cv::resize(mat, mat, cv::Size(m.rect.width, m.rect.height), 0.0, 0.0, cv::INTER_CUBIC);
            if (m.opacity > 0 && m.opacity < 100)
            {
                float fop = ((float)m.opacity)/100;
                // extract alpha to gray image
                if (mat.channels()==3)
                {
                    cv::Mat opacity_image(mat.size(), CV_8UC1, cv::Scalar::all(fop*255));
                    cv::cvtColor(mat, mat, COLOR_BGR2BGRA);
                    cv::insertChannel(opacity_image, mat, 3);
                    m.ctx.ftype = materialcontext::FT_BGRA;
                }
                else
                {
                    // extract alpha image
                    int from_to[] = {3, 0};
                    cv::Mat gray_image(mat.size(), CV_8UC1);
                    mixChannels(&mat, 1, &gray_image, 1, from_to, 1);
                    // multiply by opactity
                    gray_image.convertTo(gray_image, CV_8UC1, fop);
                    // insert back
                    cv::insertChannel(gray_image, mat, 3);
                }
            }
            if (m.rotation % 360)
            {
                cv::Point pos(m.rect.x, m.rect.y);
                mat = RotateMat(mat, pos, m.rotation % 360);
                m.rect = cv::Rect(pos.x, pos.y, mat.cols, mat.rows);
                m.ctx.ftype = materialcontext::FT_BGRA;
            }
            m.opacity = 100;
            m.rotation = 0;
            m.ctx.frames.push_back(mat.clone());
            m.ctx.w = mat.cols;
            m.ctx.h = mat.rows;
            m.ctx.fps = 0;
            LOG_INFO("imread on %s got one frame with size=[%d,%d], type=%d, channel=%d", m.path, m.ctx.w, m.ctx.h, mat.type(), mat.channels());
            break;
        case material::MT_Clock:
            {
                auto strs = splitString(m.path, ',');
                if (strs.size() != 4)
                {
                    LOG_ERROR("clock must have exactly four images, for backgound, hour/minute/second pointers");
                    return -1;
                }
                cv::Rect newRect = m.rect;
                for (int i=0; i<strs.size(); i++)
                {
                    auto &s = strs[i];
                    mat = cv::imread(s.c_str(), IMREAD_UNCHANGED); // without apha channel
                    if(mat.empty())
                    {
                        LOG_ERROR("Error opening clock image file %s", s.c_str());
                        return -1;
                    }
                    if(mat.channels() == 4)
                    {
                        // currently we only support 8bit color
                        if(mat.type()!=CV_8UC4 && mat.type()!=CV_8UC3)
                        {
                            mat.convertTo(mat, CV_8UC4, 1.0/255, 0);
                        }
                        if (mat.type()==CV_8UC3)
                        {
                            LOG_ERROR("Warning: image has 4 channels but type is CV_8UC3");
                        }
                        m.ctx.ftype = materialcontext::FT_BGRA;
                    }
                    else
                    {
                        if(mat.type()!=CV_8UC3)
                        {
                            mat.convertTo(mat, CV_8UC3, 1.0/255, 0);
                        }
                        m.ctx.ftype = materialcontext::FT_BGR;
                    }
                    if (i==0)
                    {
                        if (m.rect.width <= 0 || m.rect.height <= 0)
                        {
                            m.ctx.clock_x_ratio = x_ratio;
                            m.ctx.clock_y_ratio = y_ratio;
                            m.rect.width = mat.cols * x_ratio;
                            m.rect.height = mat.rows * y_ratio;
                        }
                        else
                        {
                            m.ctx.clock_x_ratio = ((double)m.rect.width) / mat.cols;
                            m.ctx.clock_y_ratio = ((double)m.rect.height) / mat.rows;
                            if (m.rect.width != mat.cols || m.rect.height != mat.rows)
                                cv::resize(mat, mat, cv::Size(m.rect.width, m.rect.height), 0.0, 0.0, cv::INTER_CUBIC);
                        }
                    }
                    // else
                    // {
                    //     int x = mat.cols * m.ctx.clock_x_ratio;
                    //     int y = mat.rows * m.ctx.clock_y_ratio;
                    //     if (x != mat.cols || y != mat.rows)
                    //         cv::resize(mat, mat, cv::Size(x, y), 0.0, 0.0, cv::INTER_CUBIC);
                    //     cv::Mat mat2(cv::Size(m.rect.width, m.rect.height), mat.type(), cv::Scalar::all(0.0));
                    //     cv::Mat fg, bg;
                    //     GetOverlapMatries(bg, fg, mat2, mat, (mat2.cols-mat.cols)/2, (mat2.rows-mat.rows)/2, mat.cols, mat.rows);
                    //     fg.copyTo(bg);
                    //     mat = mat2;
                    // }
                    LOG_INFO("Got clock image %d:%s, w=%d, h=%d, channels=%d, type=%d", i, s.c_str(), mat.cols, mat.rows, mat.channels(), mat.type());

                    if (m.opacity > 0 && m.opacity < 100)
                    {
                        float fop = ((float)m.opacity)/100;
                        // extract alpha to gray image
                        if (mat.channels()==3)
                        {
                            cv::Mat opacity_image(mat.size(), CV_8UC1, cv::Scalar::all(fop*255));
                            cv::cvtColor(mat, mat, COLOR_BGR2BGRA);
                            cv::insertChannel(opacity_image, mat, 3);
                        }
                        else
                        {
                            // extract alpha image
                            int from_to[] = {3, 0};
                            cv::Mat gray_image(mat.size(), CV_8UC1);
                            mixChannels(&mat, 1, &gray_image, 1, from_to, 1);
                            // multiply by opactity
                            gray_image.convertTo(gray_image, CV_8UC1, fop);
                            // insert back
                            cv::insertChannel(gray_image, mat, 3);
                        }
                    }
                    if (m.rotation % 360)
                    {
                        cv::Point pos(m.rect.x, m.rect.y);
                        mat = RotateMat(mat, pos, m.rotation % 360);
                        if (i == 0)
                            newRect = cv::Rect(pos.x, pos.y, mat.cols, mat.rows);
                    }

                    m.ctx.frames.push_back(mat.clone());
                }
                m.rect = newRect;
                m.rotation = 0;
                m.opacity = 100;
                m.ctx.ftype = m.ctx.frames[0].channels()==3? materialcontext::FT_BGR : materialcontext::FT_BGRA;
            }
            break;
        default: // including main video
            m.ctx.w = m.ctx.h = m.ctx.fps = 0;
            m.ctx.ftype = materialcontext::FT_None;
            break;
        }
        m.ctx.cts = 0.0f;
        m.ctx.tstep = m.ctx.fps? (1000.0f/m.ctx.fps) : 0.0f;
        m.ctx.glTexture = 0;
        if (m.rect.width == 0)
            m.rect.width = m.ctx.w * x_ratio;
        if (m.rect.height == 0)
            m.rect.height = m.ctx.h * y_ratio;
    }
    return 0;
}

#define AUDIO_DELTA  64

int read_next_audio(material &m, vector<unsigned char> &buf, int max_size)
{
    int product_id;
    return read_thread_stream_audio_frame(m.ctx.reader, buf, product_id, max_size);
}

// read a proper frame based on the current timestamp
cv::Mat *read_next_frame(material &m, double ts, bool disable_opengl)
{
    cv::Mat _mat;
    cv::Mat *pmat = nullptr;

    switch(m.type)
    {
    case material::MT_Gif:
        if(m.ctx.frames.size()==0)
        {
            break;
        }
        else
        {
            auto cur = m.ctx.findex%m.ctx.fps_times.size();
            if (m.ctx.cts + m.ctx.fps_times[cur] > ts) // use old
            {
                pmat = &m.ctx.frames[cur];
                break;
            }
            while (m.ctx.cts + m.ctx.fps_times[cur] < ts) // advance to proper new
            {
                m.ctx.cts += m.ctx.fps_times[cur];
                m.ctx.findex ++;
                cur = m.ctx.findex%m.ctx.fps_times.size();
            }
            // use new
            pmat = &m.ctx.frames[cur];
        }
        break;
    case material::MT_Video:
        if (m.ctx.frames.size()==0) // not yet read
        {
            std::vector<unsigned char> *buffer = NULL;
            int raw_frame_index = -1;
            int ret = read_thread_stream_video_frame(m.ctx.reader, &buffer, raw_frame_index);
            if(ret < 0)
            {
                char errbuf[128];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Read video frame error: %s, file: %s", errbuf, m.path);
                break;
            }
            if(buffer == NULL) // data not arrived
                break;
            m.ctx.w = m.ctx.reader->disp_width;
            m.ctx.h = m.ctx.reader->disp_height;
            // adjust fps
            if (m.ctx.fps < 1.0)
            {
                if (m.ctx.reader->fps < 1.0)
                    LOG_ERROR("Warning: stream is openned but fps is 0, reset to 25, stream: %s", m.path);
                m.ctx.fps = m.ctx.reader->fps >= 1.0? m.ctx.reader->fps : 25.0;
                m.ctx.tstep = 1000.0/m.ctx.fps;
                m.ctx.cts = ts;
            }
            _mat = get_bgra_mat(m.ctx.reader, m, buffer->data(), AV_PIX_FMT_BGR24, disable_opengl, NULL, NULL);
            m.ctx.frames.push_back(_mat.clone());
            m.ctx.cts += m.ctx.tstep;
            pmat = &m.ctx.frames[0];
            m.ctx.bufindex = 1;
            break;
        }
        else
        {
            bool is_stream = check_is_stream(m);
            bool use_old = true;
            while (m.ctx.cts + m.ctx.tstep < ts)
            {
                use_old = false;
                std::vector<unsigned char> *buffer = NULL;
                int raw_frame_index = -1;
                int ret = read_thread_stream_video_frame(m.ctx.reader, &buffer, raw_frame_index);
                if(ret < 0)
                {
                    char errbuf[128];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_ERROR("Error: read video frame error: %s", errbuf);
                }
                if(buffer)
                    _mat = get_bgra_mat(m.ctx.reader, m, buffer->data(), AV_PIX_FMT_BGR24, disable_opengl, NULL, NULL);
                if(buffer==NULL || _mat.empty())
                {
                    LOG_INFO("Warning: read_thread_stream_video_frame returns empty, cts=%f, ts=%f", m.ctx.cts, ts);
                    while(m.ctx.cts + m.ctx.tstep < ts) // catch up with ts
                        m.ctx.cts += m.ctx.tstep;
                    break;
                }
                m.ctx.cts += m.ctx.tstep;
            }
            if(!_mat.empty())
            {
                _mat.copyTo(m.ctx.frames[0]);
                pmat = &m.ctx.frames[0];
                m.ctx.bufindex = 1;
                break;
            }
            if (use_old) // time not arrived, use previous frame
            {
                pmat = &m.ctx.frames[(m.ctx.bufindex-1)%m.ctx.frames.size()];
                break;
            }
        }
        break;
    
    case material::MT_Time:
        if (m.ctx.frames.empty())
        {
            m.ctx.fps = 1.0; // 1 frame per second
            m.ctx.tstep = 1000.0;
            m.ctx.cts = ts;
            std::string fmt = m.text;
            replace_all(fmt, "%20%", " ");
            replace_all(fmt, "%c%", ":");
            strncpy(m.text, fmt.c_str(), sizeof(m.text));
        }
        if (m.ctx.frames.empty() || m.ctx.cts + m.ctx.tstep < ts)
        {
            time_t rawtime = time(NULL);
            struct tm *info = localtime(&rawtime);
            char buffer[1024];
            strftime(buffer, sizeof(buffer), m.text, info);
            m.ctx.frames.clear();
            get_text_image(m, buffer);
            m.ctx.cts += m.ctx.tstep;
        }
        pmat = &m.ctx.frames[0];
        break;

    case material::MT_Image:
    case material::MT_Text:
        pmat = &m.ctx.frames[0];
        break;
    case material::MT_Clock:
        if (m.ctx.frames.size() >= 4)
        {
            if (m.ctx.frames.size() == 4)
            {
                m.ctx.tstep = 1000.0; // 1 second
                m.ctx.cts = ts;
            }
            if (m.ctx.frames.size() <= 4 || m.ctx.cts + m.ctx.tstep < ts)
            {
                cv::Mat mat = m.ctx.frames[0].clone();
                time_t rawtime = time(NULL);
                struct tm *info = localtime(&rawtime);
                int rotation_sec = info->tm_sec * 6;
                int rotation_min = info->tm_min * 6 + info->tm_sec / 10;
                int rotation_hour = info->tm_hour * 30 + info->tm_min / 2;
                int rotations[] = {rotation_hour, rotation_min, rotation_sec};
                for (int i=1; i<4; i++)
                {
                    // for i: 1-hour, 2-minute, 3-second
                    cv::Point pos(0, 0);
                    auto mat2 = RotateMat(m.ctx.frames[i].clone(), pos, rotations[i-1]);
                    cv::resize(mat2, mat2, cv::Size(mat2.cols*m.ctx.clock_x_ratio, mat2.rows*m.ctx.clock_y_ratio), 0.0, 0.0, m.ctx.clock_x_ratio<1.0? cv::INTER_AREA : cv::INTER_CUBIC);
                    OverlapImageBGRA(mat, mat2, (mat.cols-mat2.cols)/2, (mat.rows-mat2.rows)/2, mat2.cols, mat2.rows);
                }
                if (m.ctx.frames.size() < 5)
                    m.ctx.frames.push_back(mat);
                else
                    m.ctx.frames[4] = mat;
                m.ctx.cts += m.ctx.tstep;
            }
            pmat = &m.ctx.frames[4];
            break;
        }
        LOG_ERROR("clock image list is not enough for clock, image count %s, expect 4", m.ctx.frames.size());
        break;

    default:
        break;
    }
    return pmat;
}

std::string format_ffmepg_encode_cmdline(const std::string ffmpeg, const char *output_video,
                    int output_width, int output_height, int fps, int bitrate, bool use_nvenc,
                    const char *encode_preset, const char *output_fmt,
                    const char *rawaudio_file, int rawaudio_channel, int64_t rawaudio_samplerate,
                    const char *rtmpurl, bool quiet)
{
    std::string audio_input;
    if (rawaudio_file)
        audio_input = std::string("-f s16le -ar ")+std::to_string(rawaudio_samplerate)+" -ac "+std::to_string(rawaudio_channel)+" -i "+rawaudio_file;
    bool has_alpha = (strncasecmp(output_fmt, "mov", 4)==0 || strncasecmp(output_fmt, "webm", 5)==0);
    bool has_mp4alpha = strncasecmp(output_fmt, "mp4alpha", 9)==0;
    std::string in_fmt = has_mp4alpha? "bgr24" : (has_alpha? "bgra" :  "yuv420p");
    int video_w = output_width, video_h = output_height;
    std::string video_input = "-f rawvideo -pixel_format "+in_fmt+" -video_size "+std::to_string(video_w)+"x"+std::to_string(video_h)+" -i -";
    std::string outparam = "-c:v copy";
    std::string filterparam = rawaudio_file? "-map 0:v -map 1:a" : ""; 
    // enocode mp4 as 48k audio to fix weichat-windows play delay and other compatibility problems
    if (rtmpurl && (output_width <= 640 || output_height <= 640))
        rawaudio_samplerate = 16000;
    else if (strncasecmp(output_fmt, "mp4", 3)==0)
        rawaudio_samplerate = 48000;
    std::string aoutparam;
    if (rawaudio_file)
        aoutparam = "-acodec aac -ar "+std::to_string(rawaudio_samplerate)+" -ac "+std::to_string(rawaudio_channel);
    std::string outfile = string("-y ") + output_video;
    if (rtmpurl==NULL && strncasecmp(output_fmt, "mov", 4)==0)
    {
        outparam = "-c:v qtrle -pix_fmt argb -threads 8 -b:v "+std::to_string(bitrate);
    }
    else if(rtmpurl==NULL && strncasecmp(output_fmt, "webm", 5)==0)
    {
        outparam = "-c:v libvpx-vp9 -pix_fmt yuva420p -threads 8 -deadline good -cpu-used 3 -tile-columns 6 -frame-parallel 1 -row-mt 1 -b:v "+std::to_string(bitrate);
        if (rawaudio_file)
            aoutparam = "-c:a libopus -b:a "+std::to_string(rawaudio_samplerate);
    }
    else // mp4 by default
    {
        if (rtmpurl)
        {
            outfile = "-f flv " + string(rtmpurl);
        }
        outparam = "-c:v "+std::string(use_nvenc? "h264_nvenc" : (rtmpurl? "h264" : "libx264"))+" -framerate "+std::to_string(fps)+" -b:v "+std::to_string(bitrate);
        if(encode_preset && encode_preset[0])
        {
            outparam += std::string(" -preset ") + encode_preset;
        }
        else if (rtmpurl)
        {
            outparam += " -preset veryfast";
        }
#ifdef FFMPEG_ENCODE_COLORSPACE
        if (rtmpurl==NULL)
            outparam += " -color_range tv -colorspace bt709 -color_trc bt709 -color_primaries bt709";
#endif
    }
    if (quiet)
        outparam += " -loglevel warning";

    char cmd[4000];
    snprintf(cmd, sizeof(cmd)-1, "%s -fflags +genpts -r %d %s %s %s %s %s %s",
             ffmpeg.c_str(), fps, video_input.c_str(), audio_input.c_str(), filterparam.c_str(), outparam.c_str(), aoutparam.c_str(), outfile.c_str());
    return cmd;
}

int start_streamout_thread(streamoutinfo *stream, const std::string &ffmpeg)
{
    if (!stream->disable_audio)
    {
        int ret = mkfifo(stream->audio_fifo, S_IRUSR|S_IWUSR);
        if (ret && errno != EEXIST)
        {
            LOG_ERROR("Error creating audio fifo %s: errno=%d:%s", stream->audio_fifo, errno, strerror(errno));
            return -1;
        }
    }
    auto cmd = format_ffmepg_encode_cmdline(ffmpeg, "-", stream->videoinfo.w, stream->videoinfo.h,
            stream->videoinfo.fps, stream->videoinfo.bitrate, false, NULL, "mp4alpha", // bgr24
            stream->disable_audio? NULL : stream->audio_fifo, stream->audioinfo.channel,
            stream->audioinfo.samplerate, stream->rtmpout, true);

    if (!stream->video_thread)
        stream->video_thread = new FFVideoEncodeThread();
    if (stream->disable_audio)
    {
        stream->video_thread->START(cmd.c_str(), NULL, "");
    }
    else
    {
        if (!stream->audio_thread)
            stream->audio_thread = new FFAudioEncodeThread();
        stream->video_thread->START(cmd.c_str(), stream->audio_thread, stream->audio_fifo);
    }
    return 0;
}

int stop_streamout_thread(streamoutinfo *stream, bool force)
{
    if (stream->video_thread)
    {
        stream->video_thread->STOP(force);
    }
    if (stream->audio_thread)
    {
        stream->audio_thread->STOP(force);
    }
    return 0;
}

int close_streamout_thread(streamoutinfo *stream, bool force)
{
    if (stream->video_thread)
    {
        stream->video_thread->EXIT(force);
        delete stream->video_thread;
        stream->video_thread = NULL;
    }
    if (stream->audio_thread)
    {
        stream->audio_thread->EXIT(force);
        // delete stream->audio_thread;
        // stream->audio_thread = NULL;
    }
    return 0;
}

struct open_materials_param
{
    std::vector<material> mlist;
    double x_ratio;
    double y_ratio;
    rawaudioinfo *rawaudio;
    material *mainaudio;
    int stream_buffer_size;
    int fps;
    bool disable_opengl;
    int product_id;
};

static std::atomic_uint64_t pmlist;

void open_materials_thread_run(open_materials_param *param)
{
    auto ret = open_materials(param->mlist, param->x_ratio, param->y_ratio, param->rawaudio, param->mainaudio, param->stream_buffer_size, param->fps, param->disable_opengl, param->product_id);
    if (ret >= 0)
    {
        for (int k=0; k<param->mlist.size(); k++)
        {
            auto &m = param->mlist[k];
            ret = 0;
            send_event(ET_MATERIAL_ADD_SUCC, std::string("Add material ")+m.path+" successfully");
        }
        // wait until the main thread read and reset the value
        uint64_t old = 0;
        auto val = ((uint64_t) new std::vector<material>(param->mlist));
        while (!std::atomic_compare_exchange_strong(&pmlist,  &old, val))
        {
            old = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    else
    {
        for (int k=0; k<param->mlist.size(); k++)
        {
            auto &m = param->mlist[k];
            close_material(m);
            send_event(ET_MATERIAL_ADD_FAIL, std::string("Add material ")+m.path+" fail");
        }
    }
    delete param;
}

int submit_open_materials(std::vector<material> &new_mlist, double x_ratio, double y_ratio, rawaudioinfo &rawaudio, material &mainaudio, int stream_buffer_size, int fps, bool disable_opengl, int product_id)
{
    open_materials_param *param = new open_materials_param;
    param->mlist = new_mlist;
    param->x_ratio = x_ratio;
    param->y_ratio = y_ratio;
    param->rawaudio = &rawaudio;
    param->mainaudio = &mainaudio;
    param->stream_buffer_size = stream_buffer_size;
    param->fps = fps;
    param->disable_opengl = disable_opengl;
    param->product_id = product_id;
    std:thread th(open_materials_thread_run, param);
    th.detach();
    return 0;
}


std::vector<material> *check_open_materials()
{
    uint64_t zero(0);
    uint64_t val = std::atomic_exchange(&pmlist, zero);
    return (std::vector<material> *)val;
}
