#pragma once
#include <string>
#include <opencv2/opencv.hpp>

class IWaterMark {
public:
    virtual void setFps(int fps) = 0;
    virtual void draw(cv::Mat &frame) = 0;
    virtual ~IWaterMark() {}

    static IWaterMark* create(const std::string& options);
};