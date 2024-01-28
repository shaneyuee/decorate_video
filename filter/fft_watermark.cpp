#include "fft_watermark.h"
#include "../3rd/log/LOGHelp.h"
#include <opencv2/imgproc/types_c.h>
#include <chrono>

using namespace cv;

#undef __MODULE__
#define __MODULE__ "FFTWaterMark"

cv::Mat FFTWaterMark::addTextWatermarkColorImage(const cv::Mat &inputImage, const std::string &text)
{
	int col = inputImage.cols;
	int row = inputImage.rows;
	cv::Mat resultshow;

	cv::Mat channelsrc[3], channelresult[3];
	split(inputImage, channelsrc);
	channelresult[0] = addTextWatermarkSingleChannel(channelsrc[0], text);
	channelresult[1] = addTextWatermarkSingleChannel(channelsrc[1], text);
	channelresult[2] = addTextWatermarkSingleChannel(channelsrc[2], text);
	merge(channelresult, 3, resultshow);

	return resultshow;
}

cv::Mat FFTWaterMark::addTextWatermarkSingleChannel(const cv::Mat &input, const std::string &text)
{
	// 根据照片大小设置水印大小宽度
	double textSize = 0.0;
	int textWidth = 0;

	int minImgSize = input.rows > input.cols ? input.cols : input.rows;

	if (minImgSize < 150)
	{
		textSize = 1.0;
		textWidth = 1;
	}
	else if (minImgSize >= 150 && minImgSize < 300)
	{
		textSize = 1.5;
		textWidth = 2;
	}
	else if (minImgSize >= 300 && minImgSize < 400)
	{
		textSize = 2.5;
		textWidth = 3;
	}
	else if (minImgSize >= 400 && minImgSize < 650)
	{
		textSize = 3.0;
		textWidth = 3;
	}
	else if (minImgSize >= 650 && minImgSize < 1000)
	{
		textSize = 4.0;
		textWidth = 4;
	}
	else if (minImgSize >= 1000)
	{
		textSize = 4.5;
		textWidth = 5;
	}
	// 将输入图像进行边界扩展，使其大小为最优的离散傅里叶变换尺寸
	int m = cv::getOptimalDFTSize(input.rows);
	int n = cv::getOptimalDFTSize(input.cols);

	cv::Mat dst;
	copyMakeBorder(input, dst, 0, m - input.rows, 0, n - input.cols, cv::BORDER_CONSTANT, cv::Scalar::all(0));

	// 将边界扩展后的图像分为实部和虚部两个通道，合并为一个复数矩阵。
	cv::Mat planes[] = {cv::Mat_<float>(dst), cv::Mat::zeros(dst.size(), CV_32F)};
	cv::Mat complete; // 二通道：实部+虚部
	merge(planes, 2, complete);
	dft(complete, complete); // 对复数矩阵进行离散傅里叶变换。

	/*
	//计算离散傅里叶变换后的矩阵的最小值和最大值。
	double minv = 0.0, maxv = 0.0;
	double* minp = &minv;
	double* maxp = &maxv;
	cv::minMaxIdx(complete, minp, maxp);
	std::cout << minv << "  " << maxv << std::endl;

	// 添加水印文字————中心对称
	// 根据离散傅里叶变换后的矩阵的均值，确定水印文字的颜色。
	double meanvalue = cv::mean(complete)[0], num;
	if (meanvalue > 128)
	{
		num = -log(abs(minv));
	}
	else
	{
		num = log(abs(maxv));
	}
	std::cout << "meanvalue:" << meanvalue << "  num:" << num << " textWidth:" << textWidth << " textSize:" << textSize << "" << std::endl;
	*/
	double num = 0.0; // CV_32F 本身是一个很大的数，这里计算num没有太大意义，0.0是一个近似中间值
	// 在离散傅里叶变换后的矩阵中心对称位置添加水印文字。
	auto pt = cv::Point(input.cols * 0.10, input.rows * 0.18);
	putText(complete, text, pt,
			cv::FONT_HERSHEY_PLAIN, textSize, cv::Scalar(num, num, num), textWidth);
	flip(complete, complete, -1);
	putText(complete, text, pt,
			cv::FONT_HERSHEY_PLAIN, textSize, cv::Scalar(num, num, num), textWidth);
	flip(complete, complete, -1);

	// 傅里叶逆变换
	idft(complete, complete);
	// 将傅里叶逆变换后的矩阵进行分离，得到实部图像。
	split(complete, planes);
	// 计算实部图像的幅度，得到最终的结果。
	magnitude(planes[0], planes[1], planes[0]);
	cv::Mat result = planes[0];
	result = result(cv::Rect(0, 0, input.cols, input.rows));
	// 在进行归一化之前各点的像素值都特别大，归一化之后转换到【0,1】
	normalize(result, result, 0, 1, CV_MINMAX);

	return result;
}

void FFTWaterMark::draw(cv::Mat &mat)
{
	if (text_.empty() || interval_ == 0 || fps_ == 0)
		return;
	if ((frame_cnt_++ % (fps_ * interval_)) != 0 && interval_ > 0)
		return;
	if (mat.type() != CV_8UC3)
	{
		LOG_ERROR("mat type %d is not CV_8UC3", mat.type());
		return;
	}
	using namespace std::chrono;
	auto start_time = system_clock::now();
	auto result = addTextWatermarkColorImage(mat, text_);
	cv::Mat fgray = result * 255;
	cv::Mat urgb;
	fgray.convertTo(urgb, CV_8UC3);
	mat = urgb;
	LOG_DEBUG("draw %s at frame %d, cost %ld ms", text_.c_str(), frame_cnt_,
			  duration_cast<milliseconds>(system_clock::now() - start_time).count());
}
