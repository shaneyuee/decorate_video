#pragma once
#include <string>

enum EventCode
{
    ET_PUSH_FAILURE = 2001,
    ET_INIT_FAILURE = 2002,
    ET_READ_FAILURE = 2003, // read mainvideo/rawvideo error, abnormal exit
    ET_STREAM_NONEXIST = 2004,
    ET_MATERIAL_ADD_FAIL = 2010,
    ET_MATERIAL_DEL_FAIL = 2011,
    ET_MATERIAL_MOD_FAIL = 2012,
    ET_INIT_SUCCESS = 2501,
    ET_MATERIAL_ADD_SUCC = 2510,
    ET_MATERIAL_DEL_SUCC = 2511,
    ET_MATERIAL_MOD_SUCC = 2512,
    ET_START_OF_STREAM = 2513,
    ET_END_OF_STREAM = 2514,
};

static struct {
    EventCode e;
    const char *desc;
} event_codes[] = {
    {ET_PUSH_FAILURE, "Write failure"},
    {ET_INIT_FAILURE, "Init failure"},
    {ET_READ_FAILURE, "Read failure"},
    {ET_STREAM_NONEXIST, "Stream not exist"},
    {ET_MATERIAL_ADD_FAIL, "Material add failure"},
    {ET_MATERIAL_DEL_FAIL, "Material delete failure"},
    {ET_MATERIAL_MOD_FAIL, "Material modify failure"},
    {ET_INIT_SUCCESS, "Init success"},
    {ET_MATERIAL_ADD_SUCC, "Material add success"},
    {ET_MATERIAL_DEL_SUCC, "Material delete success"},
    {ET_MATERIAL_MOD_SUCC, "Material modify success"},
    {ET_START_OF_STREAM, "Start of streaming"},
    {ET_END_OF_STREAM, "End of streaming"},
};

enum MsgCommand
{
    // media channel
    EC_RAWMEDIA_RAWVIDEO = 0, // video with no header
    EC_RAWMEDIA_VIDEO = 1,
    EC_RAWMEDIA_AUDIO = 2,
    EC_RAWMEDIA_EXT_AUDIO = 3, // audio with product id, for dynamically switching products, used in mainaudio+rawaudio

    // event channel
    EC_EVENT_DECORATE_SDK = 5,

    // command channel
    EC_CMD_ADD_MATERIAL = 12,  // add realtime material for streaming
    EC_CMD_DEL_MATERIAL = 13,  // delete realtime material
    EC_CMD_MOD_MATERIAL = 14,  // modify realtime material
    EC_CMD_SUBSTREAM_OUT = 15,  // add or modify substream output
    EC_CMD_STOP_SUBSTREAM = 16, // stop substream output
};

struct MsgHead
{
    int ver;  // must be 1
    int type; // one of EventCommand
    int len;
};

struct ExtMsgHeadExtAudio // for ext audio (type == 11)
{
    int product_id;
};


int init_fifo_event(const char *pipe_name);
int finish_fifo_event();

int send_event(EventCode code, const std::string &msg);

