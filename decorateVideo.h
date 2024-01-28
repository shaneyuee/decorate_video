#ifndef __DECORATE_VIDEO_HEADER__
#define __DECORATE_VIDEO_HEADER__

#define MAX_VIDEO_BUFFERED_FRAMES 500

//#define FFMPEG_DECODE_COLORSPACE // set colorspace on decoding
#define FFMPEG_ENCODE_COLORSPACE   // encode with bt709


enum DEC_ERROR_CODE
{
    DEC_ERROR_BAD_ARGUMENT = -1,
    DEC_ERROR_BAD_STREAM_FORMAT = -2,
};

#endif
