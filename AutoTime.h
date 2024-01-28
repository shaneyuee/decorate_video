#pragma once

#include <iostream>
#include <time.h>
#include <string.h>
#include <sys/time.h>

class AutoTime {
public:
    AutoTime(const char *tag, int line, const char *func, int noprint) {
        mLine = line;
        mNoPrint = noprint;
        if(!mNoPrint) {
            mTag = strdup(tag);
            mFuncName = strdup(func);
            mStartTimeMs = getTimeMs();
        }
    }

    ~AutoTime() {
        if(!mNoPrint) {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            time_t t = tv.tv_sec;
            auto lt = localtime(&t);
            fprintf(stdout, "[%04d-%02d-%02d %02d:%02d:%02d.%03d]%s -- %s, %d, takes %d ms\n",
                    lt->tm_year + 1900, lt->tm_mon+1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec, tv.tv_usec/1000,
                    mTag, mFuncName, mLine, getTimeMs() - mStartTimeMs);
        }
        if (mTag) {
            free(mTag);
            mTag = NULL;
        }
        if (mFuncName) {
            free(mFuncName);
            mFuncName = NULL;
        }
    }

    void Skip() {
        mNoPrint = true;
    }

protected:
    uint32_t getTimeMs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint32_t)ts.tv_sec * 1000 + (ts.tv_nsec + 500000) / 1000000;
    }
    int mLine = 0;
    char* mFuncName = nullptr;
    char* mTag = nullptr;
    uint32_t mStartTimeMs = 0;
    int mNoPrint;
};

#define AUTOTIME(TAG) AutoTime ___t(TAG, __LINE__, __func__, 0)
#define AUTOTIMED(TAG, debug) AutoTime ___t(TAG, __LINE__, __func__, !debug)
#define AUTOTIME_SKIP() ___t.Skip()
