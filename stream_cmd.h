#pragma once
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

struct stream_cmd_info
{
    enum {
        ADD = 0,
        DEL,
        MOD,
        SUBOUT, // substream_out
        STOPSUB, // stop subout
        SWPROD, // switch product
    } operation;
    int product_id;       // ADD/DEL/MOD
    int material_id;      // ADD/DEL/MOD
    int layer;            // MOD
    struct {
        int x, y, w, h;
    } rect;               // MOD
    std::string material; // ADD/SUBOUT
};

int start_stream_cmd_thread(const char *stream_cmd_fifo, bool pure_text);
int stop_stream_cmd_thread();

int get_stream_cmds(std::vector<stream_cmd_info> &cmds);

