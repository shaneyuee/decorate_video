#include <unistd.h>
#include <fcntl.h>
#include "matops.h"
#include "AutoTime.h"
#include "material.h"
#ifdef CentOS
#define ICV_BASE 1
#include "iw/iw_core.h"
#include "iw/iw_image.h"
#include "iw/iw_image_transform.h"
#endif

using namespace std;
using namespace cv;

extern int enable_debug;

void GetOverlapMatries(cv::Mat &outBase, cv::Mat &outImage, 
                cv::Mat &base, cv::Mat &image, int x, int y, int w, int h)
{
    // check if need resize
    if((w && w != image.cols) || (h && h != image.rows))
    {
        AUTOTIMED("resize run", enable_debug);
        if(w==0) w = image.cols;
        if(h==0) h = image.rows;
        cv::resize(image, image, cv::Size(w, h), 0.0, 0.0, cv::INTER_LINEAR);
    }

    // calculate two interleaved matries
    int newx = x, newy = y, neww = image.cols, newh = image.rows;
    int imgx = 0, imgy = 0;
    if(x < 0)
    {
        newx = 0;
        neww += x;
        imgx -= x;
    }
    if(y < 0)
    {
        newy = 0;
        newh += y;
        imgy -= y;
    }
    if(newx + neww > base.cols)
    {
        neww = base.cols - newx;
    }
    if(newy + newh > base.rows)
    {
        newh = base.rows - newy;
    }

    if(neww > 0 && newh > 0)
    {
        outBase = ((newx==0 && newy==0 && neww==base.cols && newh==base.rows)? \
                    base : base(cv::Rect(newx, newy, neww, newh)));
        outImage = ((imgx==0 && imgy == 0 && neww == image.cols && newh == image.rows) ? \
                    image : image(cv::Rect(imgx, imgy, neww, newh)));
    }
}

static void GetOverlapMatries(cv::Mat &outBase, cv::Mat &outImage, cv::Mat &outMask,
                cv::Mat &base, cv::Mat &image, cv::Mat &mask, int x, int y, int w, int h)
{
    // check if need resize
    if((w && w != image.cols) || (h && h != image.rows))
    {
        AUTOTIMED("resize run", enable_debug);
        if(w==0) w = image.cols;
        if(h==0) h = image.rows;
        cv::resize(image, image, cv::Size(w, h), 0.0, 0.0, cv::INTER_LINEAR);
        cv::resize(mask, mask, cv::Size(w, h), 0.0, 0.0, cv::INTER_LINEAR);
    }

    // calculate two interleaved matries
    int newx = x, newy = y, neww = image.cols, newh = image.rows;
    int imgx = 0, imgy = 0;
    if(x < 0)
    {
        newx = 0;
        neww += x;
        imgx -= x;
    }
    if(y < 0)
    {
        newy = 0;
        newh += y;
        imgy -= y;
    }
    if(newx + neww > base.cols)
    {
        neww = base.cols - newx;
    }
    if(newy + newh > base.rows)
    {
        newh = base.rows - newy;
    }

    if(neww > 0 && newh > 0)
    {
        outBase = ((newx==0 && newy==0 && neww==base.cols && newh==base.rows)? \
                    base : base(cv::Rect(newx, newy, neww, newh)));
        outImage = ((imgx==0 && imgy == 0 && neww == image.cols && newh == image.rows) ? \
                    image : image(cv::Rect(imgx, imgy, neww, newh)));
        outMask = ((imgx==0 && imgy == 0 && neww == mask.cols && newh == mask.rows) ? \
                    mask : mask(cv::Rect(imgx, imgy, neww, newh)));
    }
}


#if 0
void OverlapImageIBGRA(cv::Mat &base, cv::Mat &image, int x, int y, int w, int h)
{
    AUTOTIMED("OverlapImageIBGRA run", enable_debug);

    cv::Mat fg, bg;
    GetOverlapMatries(bg, fg, base, image, x, y, w, h);

    if(fg.empty())
        return;

#if 1
    //
    // handle by pixel-by-pixel method
    //
    for (int ir = fg.rows - 1; ir >= 0; --ir)
    {
        int r = fg.rows - ir - 1; // normal row
        for (int c = 0; c < fg.cols; ++c)
        {
            auto a = fg.ptr<uchar>(ir, c)[3];
            if(a < 20) // alpha is zero, skip
                continue;
            else if(a > 230) // solid forground, memcpy
            {
                memcpy(bg.ptr<uchar>(r,c), fg.ptr<uchar>(ir,c), 3);
            }
            else // alpha blending
            {
                float alpha = a / 255.00; // alpha threshold value
                float beta = 1 - alpha;
                auto d = bg.ptr<uchar>(r,c);
                auto s = fg.ptr<uchar>(ir,c);
                d[0] = beta*d[0] + alpha*s[0];
                d[1] = beta*d[1] + alpha*s[1];
                d[2] = beta*d[2] + alpha*s[2];
            }
        }
    }
#else
    //
    // handle by matric operations
    //
    cv::Mat alpha(fg.size(), CV_8UC3), alphaf;
    int from_to[] = {3, 0, 3, 1, 3, 2};
    mixChannels(&fg, 1, &alpha, 1, from_to, 1);
    // convert to float between 0.0 ~ 1.0
    alpha.convertTo(alphaf, CV_32FC3, 1.0/255);

    cv::Mat fgf, bgf;
    bg.convertTo(bgf, CV_32FC3);
    cvtColor(fg, fg, COLOR_BGRA2BGR);
    fg.convertTo(fgf, CV_32FC3);

#if 1
    // Multiply the foreground with the alpha matte
    multiply(alphaf, fgf, fgf); 
 
    // Multiply the background with ( 1 - alpha )
    multiply(Scalar::all(1.0)-alphaf, bgf, bgf); 
 
    // Add the masked foreground and background.
    add(fgf, bgf, bgf); 
#else
     // Find number of pixels.
     int numberOfPixels = fgf.rows * fgf.cols * fgf.channels();

     // Get floating point pointers to the data matrices
     float* fptr = reinterpret_cast<float*>(fgf.data);
     float* bptr = reinterpret_cast<float*>(bgf.data);
     float* aptr = reinterpret_cast<float*>(alphaf.data);

     // Loop over all pixesl ONCE
     for(
       int i = 0;
       i < numberOfPixels;
       i++, fptr++, aptr++, bptr++
     )
     {
         *bptr = (*fptr)*(*aptr) + (*bptr)*(1 - *aptr);
     }
#endif
    bgf.convertTo(bg, CV_8UC3);

#endif
}
#endif

// bgra over bgra
static void OverlapBGRAImageBGRA(cv::Mat &base, cv::Mat &image, int x, int y, int w, int h)
{
    cv::Mat fg, bg;
    GetOverlapMatries(bg, fg, base, image, x, y, w, h);

    if(fg.empty())
        return;

    static __uint128_t mask1 = (((__uint128_t)0xffULL) << 120) + (((__uint128_t)0xffULL) << 88) + (((__uint128_t)0xffULL) << 56) + (((__uint128_t)0xffULL) << 24);
    static __uint128_t mask2 = (((__uint128_t)0xffffffULL) << 96) + (((__uint128_t)0xffffffULL) << 64) + (((__uint128_t)0xffffffULL) << 32) + (((__uint128_t)0xffffffULL));

    for(int r=0; r<fg.rows; r++)
    {
        int c = 0;
        int cols = fg.cols - 4;
        for(; c<=cols; c+=4)
        {
            auto pix = *(__uint128_t *)fg.ptr<uchar>(r, c);
            auto val = (pix & mask1);
            if (val == mask1) // all ones
            {
                *(__uint128_t *)bg.ptr<uchar>(r, c) = pix;
            }
            else if (val) // not all zeros
            {
                auto pix2 = fg.ptr<uchar>(r, c);
                for (auto c1 = 0; c1 < 4; c1 ++)
                {
                    float alpha = ((float)pix2[3]) / 255; // alpha threshold value
                    float beta = 1 - alpha;
                    auto dst = bg.ptr<uchar>(r, c+c1);
                    dst[0] = beta*dst[0] + alpha*pix2[0];
                    dst[1] = beta*dst[1] + alpha*pix2[1];
                    dst[2] = beta*dst[2] + alpha*pix2[2];
                    dst[3] = beta*dst[3] + alpha*pix2[3];
                    pix2 += 4;
                }
            }
        }
        for (; c<fg.cols; c++)
        {
            auto pix2 = fg.ptr<uchar>(r, c);
            float alpha = ((float)pix2[3]) / 255; // alpha threshold value
            float beta = 1 - alpha;
            auto dst = bg.ptr<uchar>(r, c);
            dst[0] = beta*dst[0] + alpha*pix2[0];
            dst[1] = beta*dst[1] + alpha*pix2[1];
            dst[2] = beta*dst[2] + alpha*pix2[2];
            dst[3] = beta*dst[3] + alpha*pix2[3];
        }
    }
}

void OverlapImageBGRM(cv::Mat &base, cv::Mat &image, cv::Mat &mask, int x, int y, int w, int h)
{
    if (base.channels() == 4)
    {
        cv::Mat imgs[2] {image, mask}, bgra;
        int from_to[] = {0, 0, 1, 1, 2, 2, 3, 3};
        if (image.channels() == 4)
            from_to[6] = 4;
        mixChannels(imgs, 2, &bgra, 1, from_to, 4);
        OverlapBGRAImageBGRA(base, bgra, x, y, w, h);
        return;
    }

    cv::Mat fg, bg, subMsk;
    GetOverlapMatries(bg, fg, subMsk, base, image, mask, x, y, w, h);

    if(fg.empty())
        return;

    static __uint128_t mask1 = ~(__uint128_t)0;
    static  __uint64_t mask2 = ~(__uint64_t)0;

    for(int r=0; r<fg.rows; r++)
    {
        int c = 0;
        int cols = fg.cols - 16;
        for(; c<=cols; c+=16)
        {
            auto msk = *(__uint128_t*)subMsk.ptr<uchar>(r, c);
            if (msk == mask1) // all ones
            {
                memcpy(bg.ptr<uchar>(r, c), fg.ptr<uchar>(r, c), 48);
            }
            else if (msk) // not all zeros
            {
                auto pmsk8 = (__uint64_t*)subMsk.ptr<uchar>(r, c);
                for (auto t = 0; t<=8; t+=8, pmsk8++)
                {
                    auto msk8 = *pmsk8;
                    if (msk8 == mask2) // all ones
                    {
                        memcpy(bg.ptr<uchar>(r, c+t), fg.ptr<uchar>(r, c+t), 24);
                    }
                    else if (msk8) // not all zeros
                    {
                        auto pix = fg.ptr<uchar>(r, c+t);
                        auto dst = bg.ptr<uchar>(r, c+t);
                        auto pm = (uchar *)pmsk8;
                        for (auto i=0; i<8; i++,pix+=3,dst+=3,pm++)
                        {
                            auto a = *pm;
                            if(a > 240) // solid forground, memcpy
                            {
                                memcpy(dst, pix, 3);
                            }
                            else if(a > 10) // alpha is zero, skip
                            {
                                float alpha = ((float)a) / 255; // alpha threshold value
                                float beta = 1-alpha;
                                dst[0] = beta*dst[0] + alpha*pix[0];
                                dst[1] = beta*dst[1] + alpha*pix[1];
                                dst[2] = beta*dst[2] + alpha*pix[2];
                            }
                        }
                    }
                }
            }
        }
        for (; c < fg.cols; c++)
        {
            auto pix = fg.ptr<uchar>(r, c);
            auto dst = bg.ptr<uchar>(r, c);
            auto a = *subMsk.ptr<uchar>(r, c);
            if(a > 240) // solid forground, memcpy
            {
                memcpy(dst, pix, 3);
            }
            else if(a > 10) // alpha is zero, skip
            {
                float alpha = ((float)a) / 255; // alpha threshold value
                float beta = 1-alpha;
                dst[0] = beta*dst[0] + alpha*pix[0];
                dst[1] = beta*dst[1] + alpha*pix[1];
                dst[2] = beta*dst[2] + alpha*pix[2];
            }
        }
    }
}

void OverlapImageBGRA(cv::Mat &base, cv::Mat &image, int x, int y, int w, int h)
{
    if (base.channels() == 4)
    {
        OverlapBGRAImageBGRA(base, image, x, y, w, h);
    }
    else
    {
        cv::Mat imgs[2] {cv::Mat(image.size(), CV_8UC3), cv::Mat(image.size(), CV_8U)};
        int from_to[] = {0, 0, 1, 1, 2, 2, 3, 3};
        mixChannels(&image, 1, imgs, 2, from_to, 4);
        OverlapImageBGRM(base, imgs[0], imgs[1], x, y, w, h);
    }
}

void OverlapImageBGR(cv::Mat &base, cv::Mat &image, int x, int y, int w, int h)
{
    AUTOTIMED("OverlapImageBGR run", enable_debug);
    cv::Mat fg, bg;
    if (base.channels()==4 && image.channels()==3)
    {
        cvtColor(image, image, COLOR_BGR2BGRA);
    }
    else if (base.channels()==3 && image.channels()==4)
    {
        cvtColor(image, image, COLOR_BGRA2BGR);
    }
    GetOverlapMatries(bg, fg, base, image, x, y, w, h);

    if(!fg.empty())
        fg.copyTo(bg);
}

// remove background color using opencv methods, and fill with a pure color
static inline int removeBackground(cv::Mat &frame, cv::Mat &result, cv::Mat &mask, 
                    cv::Scalar fill_color, cv::Scalar lower_color, cv::Scalar upper_color, bool ishsv)
{
    if (!ishsv)
    {
        // convert color to hsv
        uchar a[9] = {(uchar)lower_color[0], (uchar)lower_color[1], (uchar)lower_color[2], 
                        (uchar)upper_color[0], (uchar)upper_color[1], (uchar)upper_color[2]};
        cv::Mat m(cv::Size(2, 1), CV_8UC3, a);
        cv::Mat hsv1;
        cvtColor(m, hsv1, cv::COLOR_BGR2HSV);
        lower_color = cv::Scalar(hsv1.at<cv::Vec3b>(0, 0));
        upper_color = cv::Scalar(hsv1.at<cv::Vec3b>(0, 1));
    }

    // 预处理: Canny边缘检测
    cv::Mat blurred;
    cv::GaussianBlur(frame, blurred, cv::Size(5, 5), 0);
    cv::Mat edges;
    cv::Canny(blurred, edges, 100, 200);

    // 使用曲线渐变将边缘变得更平滑
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::dilate(edges, edges, kernel);
    cv::erode(edges, edges, kernel);

    // 将图像从BGR色彩空间转换为HSV色彩空间
    cv::Mat hsv;
    cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

    // 过滤出绿色区域
    cv::Mat greenOnly;
    cv::inRange(hsv, lower_color, upper_color, greenOnly);

    // 合并两个mask
    cv::bitwise_xor(edges, greenOnly, mask);
    cv::bitwise_and(mask, greenOnly, mask);

    // 对掩码进行膨胀操作，去除噪声
    kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(5, 5));
    cv::dilate(mask, mask, kernel);

    // 反转掩码，将绿色区域变为黑色，其余区域变为白色
    cv::bitwise_not(mask, mask);

    // 用掩码去除绿色背景
    // 使用mask去除绿幕背景
    frame.copyTo(result, mask);

    if(fill_color[0] > 0.1f || fill_color[1] > 0.1f || fill_color[2] > 0.1f)
    {
        // 填充成指定颜色
        for(int r=0; r<frame.rows; ++r)
        for(int c=0; c<frame.cols; ++c)
        {
            if(!mask.ptr<uchar>(r,c)[0])
            {
                result.ptr<uchar>(r,c)[0] = fill_color[0];
                result.ptr<uchar>(r,c)[1] = fill_color[1];
                result.ptr<uchar>(r,c)[2] = fill_color[2];
            }
        }
    }
    if(result.channels() == 4) // 转成3通道
    {
        cv::cvtColor(result, result, cv::COLOR_BGRA2BGR);
    }
    return 0;
}

static inline int removeBackground(Mat &frame, Mat &result, Mat &mask, bool alpha_result)
{
    AUTOTIMED("RemoveBackground run", enable_debug);
    Scalar lower_color = Scalar(45, 50, 50);   //Scalar(55, 60, 60);
    Scalar upper_color = Scalar(75, 255, 255); //Scalar(75, 255, 255);
    Mat oldMask;
    if(frame.channels()==4)
    {
        cv::Mat imgs[2] {cv::Mat(frame.size(), CV_8UC3), cv::Mat(frame.size(), CV_8U)};
        int from_to[] = {0, 0, 1, 1, 2, 2, 3, 3};
        mixChannels(&frame, 1, imgs, 2, from_to, 4);
        frame = imgs[0];
        oldMask = imgs[1];
    }
    int ret = removeBackground(frame, result, mask, Scalar(0,0,0), lower_color, upper_color, true);
    if (!oldMask.empty())
    {
        multiply(oldMask, mask, mask);
    }
    if (ret == 0 && alpha_result)
    {
        if (result.channels()==3)
            cvtColor(result, result, cv::COLOR_BGR2BGRA);

        int from_to[] = {0, 3};
        mixChannels(&mask, 1, &result, 1, from_to, 1);
    }
    return ret;
}

int removeBackground(Mat &frame, Mat &result)
{
    Mat mask;
    return removeBackground(frame, result, mask, true);
}

int removeBackground(Mat &frame, Mat &result, Mat &mask)
{
    return removeBackground(frame, result, mask, false);
}


void smooth_bgcolor_shane(cv::Mat &mat, uchar bgcolor[3])
{
    // 根据颜色跟背景色的差异梯度，修改对应的alpha值
    struct {
        int diff;
        int alpha;
        cv::Vec3b low, high;
    } replacelist[] = {
        {10, 0, cv::Vec3b(), cv::Vec3b()},
        {20, 10, cv::Vec3b(), cv::Vec3b()},
        {40, 50, cv::Vec3b(), cv::Vec3b()},
        {60, 100, cv::Vec3b(), cv::Vec3b()}
    };
    #define last_color ((sizeof(replacelist)/sizeof(replacelist[0])) - 1)

    for(int i=0; i<(sizeof(replacelist)/sizeof(replacelist[0])); i++)
    {
        auto diff = replacelist[i].diff;
        auto &low = replacelist[i].low;
        auto &high = replacelist[i].high;
        for(int i=0; i<3; i++)
        {
            low[i] = (bgcolor[i] > diff? bgcolor[i] - diff : 0);
            high[i] = (bgcolor[i] < (255-diff)? bgcolor[i] + diff : 255);
        }
    }

    for(int r=0; r<mat.rows; ++r)
    for(int c=0; c<mat.cols; ++c)
    {
        auto pix = mat.ptr<uchar>(r,c);
        if(!pix[3]) // skip alpha with 0
            continue;
        if(memcmp(pix, bgcolor, 3)==0)
        {
            pix[3] = 0;
            continue;
        }
        auto &lowest = replacelist[last_color].low;
        auto &highest = replacelist[last_color].high;
        if((pix[0]>=lowest[0] && pix[0]<=highest[0]) &&
            (pix[1]>=lowest[1] && pix[1]<=highest[1]) &&
            (pix[2]>=lowest[2] && pix[2]<=highest[2]))
        {
            pix[3] = replacelist[last_color].alpha;
        }
        else
        {
            continue;
        }

        for(int i=0; i < last_color; ++i)
        {
            auto &low = replacelist[i].low;
            auto &high = replacelist[i].high;

            if((pix[0]>=low[0] && pix[0]<=high[0]) &&
                (pix[1]>=low[1] && pix[1]<=high[1]) &&
                (pix[2]>=low[2] && pix[2]<=high[2]))
            {
                pix[3] = replacelist[i].alpha;
                break;
            }
        }
    }
}

static std::string find_font(const std::string &font)
{
    if (file_exists(font))
        return font;
    if (file_exists(font+".ttc"))
        return font+".ttc";
    if (file_exists(font+".ttf"))
        return font+".ttf";

    // search by fc-list command
    FILE *f = popen(("fc-list \""+font+"\"").c_str(), "r");
    if (f)
    {
        char path[1024];
        int nr = fread(path, 1, sizeof(path)-1, f);
        fclose(f);
        if (nr > 0)
        {
            path[nr] = 0;
            char *token = strchr(path, ':');
            if (token)
                *token = 0;
            if (file_exists(path))
            {
                return path;
            }
        }
    }
    return "";
}

cv::Mat text2Mat(const std::string &text, const std::string &font, int size,
                 const cv::Scalar &bgra_color, const cv::Point& point, int max_width, int max_height,
                cvx::MatTextWrapMode wrap_mode, const cv::Scalar& shade_color)
{
    if (max_width <= 0)
        max_width = 2400;
    if (max_height <= 0)
        max_height = wrap_mode == cvx::WRAP_CROP? 240 : 800;

    std::string font_path = find_font(font);
    if (font_path.empty())
    {
        font_path = find_font(""); // find the first
        if (font_path.empty())
        {
            std::cerr << "Error: font [" << font << "] is missing and no system font found for drawing text." << std::endl;
            return cv::Mat();
        }
        std::cout << "Warning: font [" << font << "] is missing, use default font " << font_path << std::endl;
    }
    else
    {
        static int i = 0;
        if (i++ < 10)
            std::cout << "Using font file " << font_path << std::endl;
    }

    cvx::CvxFont cvfont(font_path.c_str());
    if (!cvfont.isValid())
    {
        std::cerr << "Error: failed to load font file " << font_path << std::endl;
        return cv::Mat();
    }

    cv::Mat img;

    int textlen = text.length();
    cv::Point curPos = point;

    // try single line to see if it is enough, for must case, one line is ok
    int max_lineheight = point.y*2 + size + 10;
    img = cv::Mat(cv::Size(max_width, max_lineheight), CV_8UC4, cv::Scalar(0, 0, 0, 0));
    bool fit_oneline = cvx::putText(img, text, curPos, cvfont, size, bgra_color, cvx::WRAP_CROP);

    // if need multi-line, try again with max-sized maxtrix
    if (!fit_oneline && wrap_mode != cvx::WRAP_CROP)
    {
        img = cv::Mat(cv::Size(max_width, max_height), CV_8UC4, cv::Scalar(0, 0, 0, 0));
        curPos = point;
        cvx::putText(img, text, curPos, cvfont, size, bgra_color, wrap_mode);
    }

    // crop to minimal size, and resize according to size
    cv::Point newlinePos = cvfont.getNewlinePos();
    cv::Size sz(curPos.x+40, newlinePos.y+40);

    if (!fit_oneline && wrap_mode != cvx::WRAP_CROP) // no cropping width if needs wrapping
        sz.width = img.cols;

    cv::Rect rect(0,0,sz.width,sz.height);
    if (rect.width > img.cols)
    {
        static int j = 0;
        if (j++ < 10)
            std::cout << "Warning: croping mark text width from " << rect.width << " to " << img.cols << std::endl;
        rect.width = img.cols;
    }
    if (rect.height > img.rows)
    {
        static int k = 0;
        if (k++ < 10)
            std::cout << "Warning: croping mark text height from " << rect.height << " to " << img.rows << std::endl;
        rect.height = img.rows;
    }
    auto watermat = img(rect);

    if (shade_color.val[3]) // alpha channel non-zero, use shade
    {
        // extract alpha to gray image
        cv::Mat gray_image(watermat.size(), CV_8UC1);
        int from_to[] = {3, 0};
        mixChannels(&watermat, 1, &gray_image, 1, from_to, 1);

        // make threshold image from gray image
        cv::Mat binary_image;
        cv::adaptiveThreshold(gray_image, binary_image, 255, cv::ADAPTIVE_THRESH_MEAN_C, cv::THRESH_BINARY, 7, 2);

        // make shade image from shresold image 
        cv::bitwise_not(binary_image, binary_image);
        cv::Mat shade_image(watermat.size(), CV_8UC4, shade_color);
        cv::Mat mask_image(watermat.size(), CV_8UC4, cv::Scalar(0,0,0,0));
        shade_image.copyTo(mask_image, binary_image);

        // finally, merge the two
        cv::add(watermat, mask_image, watermat);
    }
    return watermat;
}

// convert a left-right or top-bottom video frame to alpha frame
//  - frame, source video frame
//  - alpha_mode, must be one of:
//   'l' - alpha at left
//   'r' - alpha at right
//   't' - alpha at top
//   'b' - alpha at bottom
cv::Mat MakeAlphaMat(cv::Mat frame, int alpha_mode, cv::Mat *pmask)
{
    cv::Mat mmat, amat;
    int a = tolower(alpha_mode);
    if (a == 'l') // alpha at left
    {
        amat = frame(cv::Rect(0, 0, frame.cols/2, frame.rows));
        mmat = frame(cv::Rect(frame.cols/2, 0, frame.cols/2, frame.rows));
    }
    else if (a == 'r') // right
    {
        amat = frame(cv::Rect(frame.cols/2, 0, frame.cols/2, frame.rows));
        mmat = frame(cv::Rect(0, 0, frame.cols/2, frame.rows));
    }
    else if (a == 't') // top
    {
        amat = frame(cv::Rect(0, 0, frame.cols, frame.rows/2));
        mmat = frame(cv::Rect(0, frame.rows/2, frame.cols, frame.rows/2));
    }
    else if (a == 'b') // bottom
    {
        amat = frame(cv::Rect(0, frame.rows/2, frame.cols, frame.rows/2));
        mmat = frame(cv::Rect(0, 0, frame.cols, frame.rows/2));
    }
    if (!amat.empty()) // alpha blending
    {
        if (pmask)
        {
            cvtColor(amat, *pmask, COLOR_BGR2GRAY);
            return mmat;
        }

        cv::Mat mats[2] = {mmat, amat};
        frame = cv::Mat(mmat.size(), CV_8UC4);
        int from_to[] = {0,0, 1,1, 2,2, 4,3};
        if (mmat.channels()==3) // 3-bgr, 4-bgra
            from_to[6] = 3;
        cv::mixChannels(mats, 2, &frame, 1, from_to, 4);
    }

    return frame;
}


// calculate a rotation mat, and put the image inside the mat
cv::Mat GetRotationMat(cv::Mat mat, cv::Point &pos)
{
    int w = mat.cols, h = mat.rows;
    cv::Point center = cv::Point(pos.x + w/2, pos.y + h/2);
    int r = sqrt(w * w/4 + h * h/4);
    int neww, newh;
    neww = newh = r*2;;

    cv::Mat rotateMat = cv::Mat::zeros(cv::Size(neww, newh), CV_8UC4);
    pos = cv::Point(center.x - r, center.y - r);
    if (mat.channels()==3)
        cv::cvtColor(mat, mat, cv::COLOR_BGR2BGRA);
    mat.copyTo(rotateMat(cv::Rect(r-w/2, r-h/2, w, h)));
    return rotateMat;
}

static void cv_rotate_90d(cv::Mat& mat, int degree)
{
#if CV_VERSION_EPOCH <= 2
    switch (degree)
    {
        case 90:
            cv::flip(mat.t(), mat, 1);
            break;
        case 180:
            cv::flip(mat, mat, -1);
            break;
        case 270:
            cv::flip(mat.t(), mat, 0);
            break;
        default:
            break; //image size will change
    }
#else
    switch(degree)
    {
    	case 90:
            cv::rotate(mat, mat, cv::ROTATE_90_CLOCKWISE);
            break;
        case 180:
            cv::rotate(mat, mat, cv::ROTATE_180);
            break;
        case 270:
            cv::rotate(mat, mat, cv::ROTATE_90_COUNTERCLOCKWISE);
            break;
        default:
            break;
    }
#endif
}

static cv::Mat RotateMat90d(cv::Mat mat, cv::Point &pos, int degree)
{
    degree = (degree % 360);
    if ((degree % 180) != 0) // 90 or 270
    {
        int w = mat.cols, h = mat.rows;
        int x = pos.x + ((w - h) / 2);
        int y = pos.y + ((h - w) / 2);
        pos = cv::Point(x, y);
    }

    cv_rotate_90d(mat, degree);
    return mat;
}

// rotate a matrix by degree, and return the new pos corresponding to the same size / same center point mat
cv::Mat RotateMat(cv::Mat mat, cv::Point &pos, int degree)
{
    if ((degree % 90)==0)
        return RotateMat90d(mat, pos, degree);

#ifdef CentOS
    // Use Intel's IPP lib, very fast for image processing in CPU
    IwiImage iwiSrc, iwiDst;
    iwiImage_Init(&iwiSrc);
    iwiImage_Init(&iwiDst);

    degree = 360 - degree; // ipp is counter clockwise
    IwiSize srcSize = {mat.cols, mat.rows}, dstSize;
    iwiRotate_GetDstSize(srcSize, (double)degree, &dstSize);
    pos = cv::Point(pos.x + ((mat.cols - dstSize.width) / 2), pos.y + ((mat.rows - dstSize.height) / 2));

    cv::Mat dstMat(cv::Size(dstSize.width, dstSize.height), mat.type());
    iwiImage_InitExternal(&iwiSrc, srcSize, ipp8u, mat.channels(), NULL, mat.data, mat.cols * mat.channels());
    iwiImage_InitExternal(&iwiDst, dstSize, ipp8u, mat.channels(), NULL, dstMat.data, dstMat.cols * mat.channels());

    Ipp64f borderValue[4] = {0.0, 0.0, 0.0, 0.0};
    auto status = iwiRotate(&iwiSrc, &iwiDst, (double)degree, ippNearest, NULL, ippBorderConst, borderValue, NULL);
    if (status != ippStsNoErr)
    {
        std::cerr << "Error: iwiRotate failed with " << status << std::endl;
        return mat;
    }

    return dstMat;
#else
    cv::Mat resultMat;
    cv::Mat xfmMat = getRotationMatrix2D(cv::Point2f(mat.cols/2, mat.rows/2), 360-degree, 1);
#if CV_VERSION_EPOCH <= 2
    int len = std::max(mat.cols, mat.rows);
    cv::warpAffine(mat, resultMat, xfmMat, cv::Size(len, len));
#else
    cv::warpAffine(mat, resultMat, xfmMat, mat.size(), 1, CV_HAL_BORDER_TRANSPARENT);
#endif
    pos = cv::Point(pos.x + ((mat.cols - resultMat.cols) / 2), pos.y + ((mat.rows - resultMat.rows) / 2));
    return resultMat;
#endif
}

cv::Mat get_bgra_mat(FFReader *video, material &m, unsigned char *data, AVPixelFormat fmt, bool disable_opengl, const char *alpha_video, cv::Mat *pmask)
{
    if (fmt != AV_PIX_FMT_BGRA && fmt != AV_PIX_FMT_BGR24 && fmt != AV_PIX_FMT_NONE)
    {
        cerr << "Error: get_bgra_mat() passed unsupported fmt " << fmt << endl;
        return cv::Mat();
    }
    cv::Mat mat;

    // force to BGRA if needs transpancy or rotation
    if (video->rotation > 0.1)
    {
        if (fabs(video->rotation - 90.0) < 1.0 || fabs(video->rotation - 270.0) < 1.0) // 90 degree
        {
            m.rect.x += (m.rect.width - m.rect.height) / 2;
            m.rect.y -= (m.rect.width - m.rect.height) / 2;
            // swap width/height
            int h = m.rect.height;
            m.rect.height = m.rect.width;
            m.rect.width = h;
        }
        m.rotation += (int)(video->rotation+0.5);
        video->rotation = 0.0;
    }
    bool needs_transpancy = (disable_opengl && 
        ((m.opacity > 0 && m.opacity < 100) || m.rotation % 360));
    if (fmt == AV_PIX_FMT_NONE)
    {
        fmt = video->out_pix_fmt;
        if (fmt == AV_PIX_FMT_RGBA)
            fmt = AV_PIX_FMT_BGRA;
        else if (fmt != AV_PIX_FMT_BGRA && fmt != AV_PIX_FMT_BGR24)
            fmt = AV_PIX_FMT_BGR24;
    }
    if (needs_transpancy)
        fmt = AV_PIX_FMT_BGRA;
    if (alpha_video)
        fmt = AV_PIX_FMT_BGR24;

    if (video->out_pix_fmt == fmt) // no fmt conversion
    {
        mat = cv::Mat(cv::Size(video->disp_width, video->disp_height),
                        fmt == AV_PIX_FMT_BGRA? CV_8UC4 : CV_8UC3, data);
    }
    else
    {
        int t = (video->out_pix_fmt == AV_PIX_FMT_RGBA || video->out_pix_fmt == AV_PIX_FMT_BGRA)? CV_8UC4 : CV_8UC3;
        int w = video->disp_width;
        int h = video->disp_height;
        int c = fmt==AV_PIX_FMT_BGRA? cv::COLOR_BGR2BGRA : cv::COLOR_BGRA2BGR;
        if (video->out_pix_fmt == AV_PIX_FMT_YUV420P)
        {
            t = CV_8U;
            h = video->disp_height + video->disp_height / 2;
            c = fmt==AV_PIX_FMT_BGRA? cv::COLOR_YUV2BGRA_I420 : cv::COLOR_YUV2BGR_I420;
        }
        else if(video->out_pix_fmt == AV_PIX_FMT_RGB24)
            c = fmt==AV_PIX_FMT_BGRA? cv::COLOR_RGB2BGRA : cv::COLOR_RGB2BGR;
        else if(video->out_pix_fmt == AV_PIX_FMT_RGBA)
            c = fmt==AV_PIX_FMT_BGRA? cv::COLOR_RGBA2BGRA : cv::COLOR_RGBA2BGR;
        mat = cv::Mat(cv::Size(w, h), t, data);
        cv::cvtColor(mat, mat, c);
    }

    cv::Rect oldRect = m.ori_video_rect;
    if (m.ori_video_rect.width == 0 || m.ori_video_rect.height == 0)
        oldRect = m.rect;
    if (alpha_video)
    {
        auto a = tolower(alpha_video[0]);
        if (a=='l' || a=='r')
            oldRect.width *= 2;
        else if (a=='t' || a=='b')
            oldRect.height *= 2;
        if (oldRect.width != mat.cols || oldRect.height != mat.rows)
            cv::resize(mat, mat, cv::Size(oldRect.width, oldRect.height), 0.0, 0.0, cv::INTER_NEAREST);
        mat = MakeAlphaMat(mat, a, pmask);
    }
    else if (needs_transpancy)
    {
        if (oldRect.width != mat.cols || oldRect.height != mat.rows)
            cv::resize(mat, mat, cv::Size(oldRect.width, oldRect.height), 0.0, 0.0, cv::INTER_NEAREST);
    }

    if (needs_transpancy)
    {
        if (m.opacity > 0 && m.opacity < 100)
        {
            bool src_has_alpha = (video->out_pix_fmt == AV_PIX_FMT_BGRA || video->out_pix_fmt == AV_PIX_FMT_RGBA || alpha_video);
            float fop = ((float)m.opacity)/100;
            if (pmask && !pmask->empty()) // output alpha in pmask
            {
                // multiply by opactity
                pmask->convertTo(*pmask, CV_8UC1, fop);
            }
            else if (mat.channels()==4 && src_has_alpha) // keep source image alpha
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
            else
            {
                cv::Mat gray_image(mat.size(), CV_8UC1, cv::Scalar(fop*255));
                if (pmask)
                    *pmask = gray_image;
                else
                {
                    if (mat.channels()==3)
                        cv::cvtColor(mat, mat, cv::COLOR_BGR2BGRA);
                    cv::insertChannel(gray_image, mat, 3);
                }
            }
        }
        if (m.rotation % 360)
        {
            if (pmask && !pmask->empty()) // dont support rotation on mask for now, so insert back
            {
                if (mat.channels()==3)
                    cv::cvtColor(mat, mat, cv::COLOR_BGR2BGRA);
                cv::insertChannel(*pmask, mat, 3);
                pmask->release();
            }
            if (m.ori_video_rect.width == 0 || m.ori_video_rect.height == 0)
                m.ori_video_rect = m.rect;
            cv::Point pos(m.ori_video_rect.x, m.ori_video_rect.y);
            mat = RotateMat(mat, pos, m.rotation % 360);
            m.rect = cv::Rect(pos, mat.size());
        }
    }

    m.ctx.ftype = (pmask && !pmask->empty())? materialcontext::FT_BGRM : \
                (mat.channels()==3? materialcontext::FT_BGR : materialcontext::FT_BGRA);
    return mat;
}


void OverlapImage(int ftype, cv::Mat &base, cv::Mat &image, cv::Mat &mask, int x, int y, int w, int h)
{
    switch (ftype)
    {
    case materialcontext::FT_BGR:
        OverlapImageBGR(base, image, x, y, w, h);
        break;
    case materialcontext::FT_BGRA:
        OverlapImageBGRA(base, image, x, y, w, h);
        break;
    case materialcontext::FT_BGRM:
        OverlapImageBGRM(base, image, mask, x, y, w, h);
        break;
    default:
        break;
    }
}

void OverlapImage(material &m, cv::Mat &base, int display_width, int display_height, cv::Mat &image, cv::Mat &mask, bool output_alpha)
{
    if (base.empty())
    {
        if (m.rect.x == 0 && m.rect.y == 0 && m.rect.width == display_width &&  m.rect.height == display_height)
        {
            if (image.cols != display_width || image.rows != display_height)
                cv::resize(image, image, cv::Size(display_width, display_width), 0, 0, INTER_LINEAR);

            if (output_alpha)
            {
                if (mask.empty())
                {
                    base = image;
                    if (image.channels() == 3)
                        cvtColor(base, base, COLOR_BGR2BGRA);
                }
                else
                {
                    base.create(cv::Size(display_width, display_height), CV_8UC4);
                    cv::Mat imgs[2] = {image, mask};
                    int from_to[] = {0, 0, 1, 1, 2, 2, image.channels(), 3};
                    mixChannels(imgs, 2, &base, 1, from_to, 4);
                }
            }
            else
            {
                base = image;
                if (image.channels() == 4)
                    cvtColor(base, base, COLOR_BGRA2BGR);
            }
        }
        else
        {
            base = cv::Mat::zeros(cv::Size(display_width, display_height), output_alpha? CV_8UC4 : CV_8UC3);
            OverlapImage(m.ctx.ftype, base, image, mask,
                m.rect.x, m.rect.y, m.rect.width, m.rect.height);
        }
    }
    else
    {
        OverlapImage(m.ctx.ftype, base, image, mask, 
            m.rect.x, m.rect.y, m.rect.width, m.rect.height);
    }
}

