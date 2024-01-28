#ifndef __uitl_tools_h__
#define __uitl_tools_h__
#include <chrono>
#include <string>
class CalculateElapsed
{
private:
    std::chrono::steady_clock::time_point m_start, m_previousTime;
    std::string m_name;
    std::string m_module;
    std::string m_extra;
public:
    CalculateElapsed(const std::string &name, const std::string &module);
    ~CalculateElapsed();
    long long Elapsed();
    void SetExtra(const std::string &extra)
    {
        m_extra = extra;
    }
};
#endif