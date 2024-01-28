/* ffmpeg decoder for gif
 * */
#include <vector>
#include <stdio.h>
#include <opencv2/opencv.hpp>
#include <webp/decode.h>
#include <webp/demux.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}


extern "C" int ImgIoUtilReadFile(const char* const file_name,
                      const uint8_t** data, size_t* data_size);

static int ReadFileToWebPData(const char* const filename,
                              WebPData* const webp_data) {
  const uint8_t* data;
  size_t size;
  if (!ImgIoUtilReadFile(filename, &data, &size)) return 0;
  webp_data->bytes = data;
  webp_data->size = size;
  return 1;
}

int decode_webp(const char *webpFile, std::vector<cv::Mat> &frames, std::vector<double> &pts)
{
    // open webp file
    FILE* webpFp = fopen(webpFile, "rb");
    if (webpFp == nullptr) {
        std::cerr << "Failed to open webp file: " << webpFile << std::endl;
        return -1;
    }

    // decode WebP animation
    WebPData webpData;
    if (!ReadFileToWebPData(webpFile, &webpData)) {
        std::cerr << "Failed to read WebP data from file: " << webpFile << std::endl;
        fclose(webpFp);
        return -1;
    }

    WebPAnimDecoderOptions decOptions;
    if (!WebPAnimDecoderOptionsInit(&decOptions)) {
        std::cerr << "Failed to init webp decoder options" << std::endl;
        WebPDataClear(&webpData);
        fclose(webpFp);
        return -1;
    }
    WebPAnimDecoder* dec = WebPAnimDecoderNew(&webpData, &decOptions);
    if (dec == nullptr) {
        std::cerr << "Failed to new webp decoder" << std::endl;
        WebPDataClear(&webpData);
        fclose(webpFp);
        return -1;
    }

    // decode each frame
    int frameIndex = 0;
    WebPAnimInfo animInfo;
    if (!WebPAnimDecoderGetInfo(dec, &animInfo)) {
        std::cerr << "Failed to get webp decoder info" << std::endl;
        WebPAnimDecoderDelete(dec);
        WebPDataClear(&webpData);
        fclose(webpFp);
        return -1;
    }

    while (WebPAnimDecoderHasMoreFrames(dec)) {
        uint8_t* frameRGBA = nullptr;
        int timestamp;
        if (WebPAnimDecoderGetNext(dec, &frameRGBA, &timestamp)) {
            printf("---webpdecode: got webp image %d with timestamp %d\n", frameIndex, timestamp);

            cv::Mat mat(cv::Size(animInfo.canvas_width, animInfo.canvas_height), CV_8UC4, frameRGBA);
            cv::cvtColor(mat, mat, cv::COLOR_RGBA2BGRA);
            frames.push_back(mat.clone());
            pts.push_back(timestamp*0.001);

            frameIndex++;
        }
    }

    // free resource
    WebPAnimDecoderDelete(dec);
    WebPDataClear(&webpData);
    fclose(webpFp);
    return 0;
}

int decode_gif(const char *file, std::vector<cv::Mat> &frames, std::vector<double> &pts)
{
    AVFormatContext *formatCtx = nullptr;
    uint8_t *displayBuffer = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *displayFrame = nullptr;
    AVCodecContext *avCondecCtx = nullptr;
    SwsContext *swsCtx = nullptr;

    int error = 0;

    do {
        if (avformat_open_input(&formatCtx, file, nullptr, nullptr) < 0)
        {
            fprintf(stderr, "avformat_open_input failed, video path:%s\n", file);
            error = -1;
            break;
        }

        if (avformat_find_stream_info(formatCtx, nullptr) < 0)
        {
            fprintf(stderr, "avformat_find_stream_info failed, video path %s.\n", file);
            error = -1;
            break;
        }

        auto ret = av_find_best_stream(formatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (ret < 0)
        {
            fprintf(stderr, "av_find_best_stream not found gif stream, path:%s\n", file);
            error = -1;
            break;
        }
        auto streamIndex = ret;
        // decoder parameters
        AVCodecParameters *codecParameters = formatCtx->streams[streamIndex]->codecpar;

        if (codecParameters->codec_id == AV_CODEC_ID_WEBP)
        {
            avformat_close_input(&formatCtx);
            return decode_webp(file, frames, pts);
        }

        AVCodec *avCode = (AVCodec *)avcodec_find_decoder(codecParameters->codec_id);
        if (!avCode)
        {
            fprintf(stderr, "Not found decoder file for gif:%s\n", file);
            error = -1;
            break;
        }
        // decoder context
        avCondecCtx = avcodec_alloc_context3(avCode);
        if (avcodec_parameters_to_context(avCondecCtx, codecParameters) != 0)
        {
            fprintf(stderr, "avcodec_parameters_to_context failed, path:%s\n", file);
            error = -1;
            break;
        }
        // open decoder
        ret = avcodec_open2(avCondecCtx, avCode, nullptr);
        if (ret < 0)
        {
            fprintf(stderr, "avcodec_open2 failed. result=(%d) path:%s\n", ret, file);
            error = -1;
            break;
        }
        auto width = avCondecCtx->width;
        auto height = avCondecCtx->height;        
        double timebase = av_q2d(formatCtx->streams[streamIndex]->time_base);
        if(timebase < 0.0001f)
        {
            double fr = av_q2d(formatCtx->streams[streamIndex]->r_frame_rate);
            fprintf(stderr, "Warning: failed to read gif time_base, use r_frame_rate of %g.\n", fr);
            timebase = 1.0/fr;
        }
        frame = av_frame_alloc();
        // image resample
        displayFrame = av_frame_alloc();
        auto bufferSize = width * height * sizeof(uint8_t) * 4;
        displayBuffer = new uint8_t[width * height * sizeof(uint8_t) * 4]();
        if (av_image_fill_arrays(displayFrame->data, displayFrame->linesize,
            displayBuffer, AV_PIX_FMT_RGBA, width, height, 1) < 0)
        {
            fprintf(stderr, "av_image_fill_arrays create dst miage fail path:%s", file);
            error = -1;
            break;
        }
        displayFrame->width = width;
        displayFrame->height = height;
        swsCtx = sws_getContext(width, height, avCondecCtx->pix_fmt,
            width, height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!swsCtx)
        {
            fprintf(stderr, "swsCtx create fail path:%s", file);
            error = -1;
            break;
        }
        while (true)
        {
            AVPacket *packet = av_packet_alloc();
            ret = av_read_frame(formatCtx, packet);
            if (ret < 0) // finished
            {
                av_packet_unref(packet);
                av_packet_free(&packet);
                error = 0;
                break;
            }
            if (packet->stream_index != streamIndex) // audio stream
            {
                av_packet_unref(packet);
                av_packet_free(&packet);
                continue;
            }
            // send and release packet
            ret = avcodec_send_packet(avCondecCtx, packet);
            av_packet_unref(packet);
            av_packet_free(&packet);
            if (ret < 0 )
            {
                error = -1;
                fprintf(stderr, "avcodec_send_packet failed, ret=%d\n", ret);
                break;
            }
            while (avcodec_receive_frame(avCondecCtx, frame) >= 0)
            {
                // convert color
                ret = sws_scale(swsCtx, frame->data,
                    frame->linesize, 0, frame->height, displayFrame->data, displayFrame->linesize);
                if (ret < 0)
                {
                    printf("sws_scale failed\n");
                    error = -1;
                    break;
                }
                printf("---gifdecode: frame pts=%lld, timebase=%g, pts_time=%g\n", frame->pts, timebase, frame->pts*timebase);

                cv::Mat mat(cv::Size(width, height), CV_8UC4, displayBuffer);
                cv::cvtColor(mat, mat, cv::COLOR_RGBA2BGRA);
                frames.push_back(mat.clone());
                pts.push_back(frame->pts*timebase);

                // release src frame
                av_frame_unref(frame);
            }
        }
    }while(0);

    if(displayBuffer)
        delete []displayBuffer;
    
    if(swsCtx)
        sws_freeContext(swsCtx);
    if(displayFrame)
        av_frame_free(&displayFrame);
    if(frame)
        av_frame_free(&frame);
    if(avCondecCtx)
        avcodec_free_context(&avCondecCtx);
    if (formatCtx)
        avformat_close_input(&formatCtx);

    return error;
}
