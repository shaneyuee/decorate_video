#include "watermark.h"
#include "fft_watermark.h"
#include "../3rd/log/LOGHelp.h"

#undef __MODULE__
#define __MODULE__ "WaterMark"

IWaterMark *IWaterMark::create(const std::string &options)
{
    // --blind_watermark=<type(text|file)>:<text|image_path>:<interval>
    size_t pos_start = 0, pos = 0;
    std::vector<std::string> items;
    while ((pos = options.find(":", pos_start)) != std::string::npos) {
        auto token = options.substr(pos_start, pos - pos_start);
        items.push_back(token);
        pos_start = pos + 1;
    }
    items.push_back(options.substr(pos_start));
    if (items.size() != 3)
    {
        LOG_ERROR("Invalid watermark format: %s", options.c_str());
        return nullptr;
    }
    if (items[0] == "text")
    {
        auto text = items[1];
        if (text.size() > 8) {
            LOG_ERROR("Text size is too long: %s", text.c_str());
            return nullptr;
        }
        int interval = atoi(items[2].c_str());
        auto wm = new FFTWaterMark();
        wm->setWatermarkText(items[1]);
        wm->setInterval(interval);
        return wm;
    }
    LOG_ERROR("Unsupport watermark type: %s", items[0].c_str());
    return nullptr;
}
