#include "LOGHelp.h"
#include <sstream>
#include <chrono>
#include <thread>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <ctime>

#define MAX_EVT_LEN	10240


std::string Replace(std::string &strtem, const std::string &s1, const std::string &s2)
{
	std::string::size_type pos = 0;
	std::string::size_type srcLen = s1.size();
	std::string::size_type desLen = s2.size();
	pos = strtem.find(s1.c_str(), pos);
	while ((pos != std::string::npos))
	{
		strtem.replace(pos, srcLen, s2.c_str());
		pos = strtem.find(s1.c_str(), (pos + desLen));
	}
	return strtem;
}

LOGHelp::LOGHelp()
{
	Init("./", LogLevel::info);
}


LOGHelp::~LOGHelp()
{
	Uninit();
}

void LOGHelp::Init(std::string strDir, LogLevel level)
{
	m_level = level;
    m_strPath = strDir;
}


std::string LOGHelp::GetThreadId()
{
	std::stringstream sin;
	auto id = std::this_thread::get_id();
	sin << id;
	return sin.str();
}

std::string LOGHelp::LogDir() const
{
	return m_strPath;
}

void LOGHelp::WriteLog(LogLevel level, const char *format, ...)
{
	if (level > m_level)
	{
		return;
	}
	char msg[MAX_EVT_LEN + 7] = { 0 };

	struct timeval tv;
	gettimeofday(&tv, NULL);
	time_t t = tv.tv_sec;
	auto lt = localtime(&t);
	int len = (int)snprintf(msg, sizeof(msg)-1, "[%04d-%02d-%02d %02d:%02d:%02d.%03d][tid:%s]",
			lt->tm_year + 1900, lt->tm_mon+1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec, tv.tv_usec/1000,
			GetThreadId().c_str());

	va_list args;
	va_start(args, format);
	int _r = vsnprintf(&(msg[len]), MAX_EVT_LEN - len, format, args);
	if (_r < 0 || _r >(MAX_EVT_LEN - len))
	{
		int maxln = MAX_EVT_LEN;
		if (msg[MAX_EVT_LEN - 1] < 0)
		{
			maxln--;
			if (msg[MAX_EVT_LEN - 2] < 0)
			{
				maxln--;
			}
		}
		strcpy(&(msg[maxln]), "...");
		len = maxln + 3;
	}
	else
	{
		len += _r;
	}

	msg[len] = '\n';
	va_end(args);
	_Write(msg, level);
}
void LOGHelp::_Write(const std::string &message, LogLevel level)
{
	fprintf(level<=error? stderr:stdout, "%s", message.c_str());
}

void LOGHelp::Uninit()
{
}

static std::time_t getTimeStamp()
{
	std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds> tp = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
	auto tmp = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch());
	std::time_t timestamp = tmp.count();
	return timestamp;
}
static std::tm *gettm(uint64_t timestamp)
{
	uint64_t milli = timestamp;
	milli += (uint64_t)8 * 60 * 60 * 1000;//转换时区，北京时间+8小时
	auto mTime = std::chrono::milliseconds(milli);
	auto tp = std::chrono::time_point<std::chrono::system_clock, std::chrono::milliseconds>(mTime);
	auto tt = std::chrono::system_clock::to_time_t(tp);
	std::tm *now = std::gmtime(&tt);
	return now;
}

std::string getTimeStr()
{
	time_t timep;
	timep = getTimeStamp();
	struct tm *info;
	info = gettm(timep);

	char tmp[27] = { 0 };
	sprintf(tmp, "[%04d-%02d-%02d %02d:%02d:%02d.%06ld]", info->tm_year + 1900, info->tm_mon + 1, info->tm_mday, info->tm_hour, info->tm_min, info->tm_sec, timep % 1000000);
	return tmp;
}

LOGHelp& GetLogHelp()
{
	static LOGHelp gLogHelp;
	return gLogHelp;
}
