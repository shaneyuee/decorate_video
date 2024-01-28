#pragma once
#include "watermark.h"


class FFTWaterMark : public IWaterMark
{
public:
    virtual void draw(cv::Mat &frame);
    virtual void setFps(int fps) { fps_ = fps; }
    virtual ~FFTWaterMark() {}
    
    void setWatermarkText(std::string text) { text_ = text; }

    // set inerval to add on frame watermark
    // @param interval in second
    void setInterval(int interval) { interval_ = interval; }


    static cv::Mat addTextWatermarkColorImage(const cv::Mat &inputImage, const std::string &text);
    static cv::Mat addTextWatermarkSingleChannel(const cv::Mat &input, const std::string &text);

private:
    std::string text_;
    int interval_ = 0;
    int fps_ = 24;
    int frame_cnt_ = 0;
};