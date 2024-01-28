#pragma once

#include <string>
#include <memory>
#include <stdio.h>
#ifndef __MODULE__
#define	__MODULE__	"unnamed"
#endif

enum LogLevel : int
{
	fatal = 0,
	error,
	info,
	debug
};

class LOGHelp
{
public:
	LOGHelp();
	~LOGHelp();
	void Init(std::string strDir, LogLevel level);
	void WriteLog(LogLevel level, const char *format, ...);
	std::string LogDir() const;
	void Uninit();
	LogLevel GetLogLevel() const { return m_level; }
private:
	void _Write(const std::string &message, LogLevel level);
	std::string GetThreadId();
private:
	LogLevel m_level;
	std::string m_strPath;
};
std::string getTimeStr();

LOGHelp& GetLogHelp();


#define writelog(level, fmt, ...) \
	GetLogHelp().WriteLog(level, fmt, ##__VA_ARGS__)
#define loglevel() GetLogHelp().GetLogLevel()

#define LOG_FATAL(fmt, ...)  do { if (loglevel() >= LogLevel::fatal) writelog(LogLevel::fatal, "[FATAL][%s:%d]" fmt ,__MODULE__, __LINE__, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(fmt, ...)  do { if (loglevel() >= LogLevel::error) writelog(LogLevel::error, "[ERROR][%s:%d]" fmt ,__MODULE__, __LINE__, ##__VA_ARGS__); } while(0)
#define LOG_INFO(fmt, ...)   do { if (loglevel() >= LogLevel::info)  writelog(LogLevel::info,  "[INFO][%s:%d]"  fmt ,__MODULE__, __LINE__, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)  do { if (loglevel() >= LogLevel::debug) writelog(LogLevel::debug, "[DEBUG][%s:%d]" fmt ,__MODULE__, __LINE__, ##__VA_ARGS__); } while(0)
