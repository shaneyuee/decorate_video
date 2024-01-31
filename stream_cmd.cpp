#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "stream_cmd.h"
#include "safequeue.h"
#include "event.h"

#ifdef MacOS
#define st_mtim st_atimespec
#endif

std::vector<std::string> splitString(const std::string &str, char sep);

class StreamCmdThread
{
public:
    StreamCmdThread() : bExit(false), bStopped(false), reader(NULL), runner(NULL), text_file_mode(false)
    {
    }
    ~StreamCmdThread()
    {
    }
    void RUN()
    {
        MsgHead header = { 0 };
        char buffer[10240];
        struct timespec prev_mtime = {0, 0};

        while(!bExit)
        {
            if (bStopped)
            {
                if (bExit) // exit
                {
                    if (reader != NULL)
                    {
                        fclose(reader);
                        reader = NULL;
                    }
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            struct stat st;
            if (text_file_mode)
            {
                int ret = stat(fifo_name.c_str(), &st);
                if (ret < 0)
                {
                    if (errno == ENOENT)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        continue;
                    }
                    fprintf(stderr, "StreamCmdThread fstat file error, file is %s, ret=%d:%s, will try again later.\n", fifo_name.c_str(), errno, strerror(errno));
                    std::this_thread::sleep_for(std::chrono::seconds(10));
                    continue;
                }
                if (st.st_mtim.tv_sec == prev_mtime.tv_sec && st.st_mtim.tv_nsec == prev_mtime.tv_nsec)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            }

            if (reader == NULL)
                reader = fopen(fifo_name.c_str(), "r");
            if (reader == NULL)
            {
                fprintf(stderr, "StreamCmdThread fopen file error, file is %s, ret=%d:%s, will try again later.\n", fifo_name.c_str(), errno, strerror(errno));
                std::this_thread::sleep_for(std::chrono::seconds(10));
                continue;
            }
            if (text_file_mode)
            {
                prev_mtime.tv_sec = st.st_mtim.tv_sec;
                prev_mtime.tv_nsec = st.st_mtim.tv_nsec;
            }
            char *line;
            while ((line = fgets(buffer, sizeof(buffer), reader)))
            {
                while (isspace(*line)) line ++;
                if (*line == '#')
                    continue;

                stream_cmd_info cmd = {stream_cmd_info::ADD, 0, 0, 0, {0, 0, 0, 0}, std::string()};
                if (strncasecmp(line, "ADD ", 4)==0 || strncasecmp(line, "ADD\t", 4)==0)
                {
                    line += 4; while (isblank(*line)) line ++;
                    cmd.operation = stream_cmd_info::ADD;
                    auto strs = splitString(line, ':');
                    if (strs.size() < 2)
                    {
                        fprintf(stderr, "StreamCmdThread bad parameters for DEL : %s\n", line);
                        continue;
                    }
                    cmd.product_id = atoi(strs[0].c_str());
                    cmd.material_id = atoi(strs[1].c_str());
                    char *s = strchr(line+strs[0].length()+strs[1].length()+1, ':');
                    if (s)
                        cmd.material = s + 1;
                    fprintf(stdout, "StreamCmd ADD: productid=%d, materialid=%d, material_spec=[%s]\n", cmd.product_id, cmd.material_id, cmd.material.c_str());
                    cmdQueue.Push(cmd);
                    continue;
                }
                if (strncasecmp(line, "DEL ", 4)==0 || strncasecmp(line, "DEL\t", 4)==0)
                {
                    line += 4; while (isblank(*line)) line ++;
                    cmd.operation = stream_cmd_info::DEL;
                    auto strs = splitString(line, ':');
                    if (strs.size() < 2)
                    {
                        fprintf(stderr, "StreamCmdThread bad parameters for DEL : %s\n", line);
                        continue;
                    }
                    cmd.product_id = atoi(strs[0].c_str());
                    cmd.material_id = atoi(strs[1].c_str());
                    fprintf(stdout, "StreamCmd DEL: productid=%d, materialid=%d\n", cmd.product_id, cmd.material_id);
                    cmdQueue.Push(cmd);
                    continue;
                }
                if (strncasecmp(line, "MOD ", 4)==0 || strncasecmp(line, "MOD\t", 4)==0)
                {
                    line += 4; while (isblank(*line)) line ++;
                    cmd.operation = stream_cmd_info::DEL;
                    auto strs = splitString(line, ':');
                    if (strs.size() < 7)
                    {
                        fprintf(stderr, "StreamCmdThread bad parameters for DEL : %s\n", line);
                        continue;
                    }
                    cmd.product_id = atoi(strs[0].c_str());
                    cmd.material_id = atoi(strs[1].c_str());
                    cmd.layer = atoi(strs[2].c_str());
                    cmd.rect.y = atoi(strs[3].c_str()); // top
                    cmd.rect.x = atoi(strs[4].c_str()); // left
                    cmd.rect.w = atoi(strs[5].c_str());
                    cmd.rect.h = atoi(strs[6].c_str());
                    fprintf(stdout, "StreamCmd MOD: productid=%d, materialid=%d, layer=%d, rect={%d,%d,%d,%d}\n",
                                    cmd.product_id, cmd.material_id, cmd.layer, cmd.rect.x, cmd.rect.y, cmd.rect.w, cmd.rect.h);
                    cmdQueue.Push(cmd);
                    continue;
                }
                if (strncasecmp(line, "SUBOUT ", 7)==0 || strncasecmp(line, "SUBOUT\t", 7)==0)
                {
                    line += 7; while (isblank(*line)) line ++;
                    cmd.operation = stream_cmd_info::SUBOUT;
                    cmd.material = line;
                    fprintf(stdout, "StreamCmd SUBOUT: stream_out_spec=[%s]\n", cmd.material.c_str());
                    cmdQueue.Push(cmd);
                    continue;
                }
                if (strncasecmp(line, "STOPSUB", 7)==0)
                {
                    cmd.operation = stream_cmd_info::STOPSUB;
                    fprintf(stdout, "StreamCmd STOPSUB\n");
                    cmdQueue.Push(cmd);
                    continue;
                }
                if (strncasecmp(line, "SWPROD ", 7)==0 || strncasecmp(line, "SWPROD\t", 7)==0)
                {
                    line += 7; while (isblank(*line)) line ++;
                    cmd.operation = stream_cmd_info::SWPROD;
                    cmd.product_id = atoi(line);
                    fprintf(stdout, "StreamCmd SWPROD\n");
                    cmdQueue.Push(cmd);
                    continue;
                }
                fprintf(stderr, "StreamCmdThread got bad command line : %s\n", line);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            fclose(reader);
            reader = NULL;
        }
    }

    void STOP()
    {
        bStopped.store(true);
    }
    void START(const std::string &fifo, bool pure_text)
    {
        fifo_name = fifo;
        bExit.store(false);
        bStopped.store(false);
        text_file_mode = pure_text;
        if (runner == NULL)
            runner = new std::thread(&StreamCmdThread::RUN, this);
    }
    void EXIT()
    {
        bExit.store(true);
        if(runner->joinable())
        {
            runner->join();
        }
        delete runner;
        runner = NULL;
    }
public:
    std::atomic_bool bStopped, bExit;
    SafeQueue<stream_cmd_info> cmdQueue;
    FILE *reader;
    std::thread *runner;
    std::string fifo_name;
    bool text_file_mode;
}streamCmdThread;


int start_stream_cmd_thread(const char *stream_cmd_fifo, bool pure_text)
{
    streamCmdThread.START(stream_cmd_fifo, pure_text);
    return 0;
}

int stop_stream_cmd_thread()
{
    streamCmdThread.EXIT();
    return 0;
}

int get_stream_cmds(std::vector<stream_cmd_info> &cmds)
{
    stream_cmd_info cmd;
    while (streamCmdThread.cmdQueue.Pop(cmd))
    {
        cmds.push_back(cmd);
    }
    return 0;
}

