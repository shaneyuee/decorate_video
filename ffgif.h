#include <vector>
#include <opencv2/opencv.hpp>

int decode_gif(const char *file, std::vector<cv::Mat> &frames, std::vector<double> &pts);
