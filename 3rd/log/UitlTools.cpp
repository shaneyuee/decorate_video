#include "UitlTools.h"
#include "LOGHelp.h"

#undef	__MODULE__
#define __MODULE__ m_module.c_str()

CalculateElapsed::CalculateElapsed(const std::string &name, const std::string &module)
{
    if (loglevel() <= LogLevel::debug)
    {
        m_start = std::chrono::steady_clock::now();
        m_previousTime = std::chrono::steady_clock::now();
        m_name = name;
        m_module = module;
    }
}

CalculateElapsed::~CalculateElapsed()
{
    LOG_DEBUG("%s%s takes %lu ms", m_name.c_str(), m_extra.c_str(), 
        std::chrono::duration_cast<std::chrono::milliseconds>
        (std::chrono::steady_clock::now() - m_start).count());
}

long long CalculateElapsed::Elapsed()
{
    if (loglevel() <= LogLevel::debug)
    {
        auto now = std::chrono::steady_clock::now();
        auto diff = std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::steady_clock::now() - m_previousTime).count();
        m_previousTime = now;
        return diff;
    }
    return 0;
}
