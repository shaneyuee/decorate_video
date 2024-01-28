//
// general matrix operations
//
#pragma once
#include <vector>
#include <opencv2/opencv.hpp>
#include "3rd/cvxfont/cvxfont.h"
#include "videoplayer.h"
#include "material.h"

// Calculate overlapping matries
void GetOverlapMatries(cv::Mat &outBase, cv::Mat &outImage, 
                cv::Mat &base, cv::Mat &image, int x, int y, int w, int h);

// Blend image with base at x/y, shrink to w/h if needed, using mask as alpha channel
void OverlapImageBGRM(cv::Mat &base, cv::Mat &image, cv::Mat &mask, int x, int y, int w, int h);
// Blend image with base at x/y, shrink to w/h if needed
void OverlapImageBGRA(cv::Mat &base, cv::Mat &image, int x, int y, int w, int h);
// Merge image on top of base
void OverlapImageBGR(cv::Mat &base, cv::Mat &image, int x, int y, int w, int h);
void OverlapImage(int ftype, cv::Mat &base, cv::Mat &image, cv::Mat &mask, int x, int y, int w, int h);
// Used when base may be empty, in that case, base will be created from image, so that no blending happens
void OverlapImage(material &m, cv::Mat &base, int display_width, int display_height, cv::Mat &image, cv::Mat &mask, bool output_alpha);

// Remove green background color using opencv methods, return BGRA result
int removeBackground(cv::Mat &frame, cv::Mat &result);
// Remove green background, return BGR result + single channel mask
int removeBackground(cv::Mat &frame, cv::Mat &result, cv::Mat &mask);

// Simple method to smooth an image
void smooth_bgcolor_shane(cv::Mat &mat, uchar bgcolor[3]);

// create a matrix and draw text in it
//  - font, must be a valid font name (fc-list can find) or font file pathname
//  - autosize, auto calculate font_size based on text length, take effect only when wrap_mode is WRAP_CROP
//  - wrap_mode, decide how to wrap around when width limit is reached
//  - shade_color, if shade_color[3] is non-zero, use shade_color[0-2]
cv::Mat text2Mat(const std::string &text, const std::string &font, int size,
                 const cv::Scalar &bgra_color, const cv::Point& point,
                 int max_width, int max_height, cvx::MatTextWrapMode wrap_mode, const cv::Scalar& shade_color);


// calculate a rotation mat, and put the image inside the mat
cv::Mat GetRotationMat(cv::Mat mat, cv::Point &pos);

// rotate a matrix by degree, and return the new pos
cv::Mat RotateMat(cv::Mat mat, cv::Point &pos, int degree);

// convert a left-right or top-bottom video frame to alpha frame
//  - frame, source video frame
//  - alpha_mode, must be one of:
//   'l' - alpha at left
//   'r' - alpha at right
//   't' - alpha at top
//   'b' - alpha at bottom
//  - pmask, if non-null, returns single channel mask and BGR frame
cv::Mat MakeAlphaMat(cv::Mat frame, int alpha_mode, cv::Mat *pmask);

cv::Mat get_bgra_mat(unsigned char *data, int length, int format, int width, int height, int out_fmt, material &m, bool disable_opengl);
cv::Mat get_bgra_mat(FFReader *video, material &m, unsigned char *data, AVPixelFormat fmt, bool disable_opengl, const char *alpha_video, cv::Mat *pmask);
