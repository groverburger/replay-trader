// Indicator calculation off the UI thread. The chart keeps drawing an indicator's last result (lined up by its
// x0) while the worker computes the next one in a Lua state of its own, so a slow script never stalls a frame.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "script.h"

struct lua_State;

struct IndJob {
    int uid = 0;                         // the indicator (Ind::uid)
    ScriptDef def;                       // a copy: the worker loads def.source into its own state
    int gen = 0;                         // g_scripts.generation() that def belongs to
    Params params;
    int tf = 1;
    long series = 0;                     // g.s_version of `s`
    std::shared_ptr<const Series> s;
};

struct IndDone {
    int uid = 0, gen = 0, tf = 1;
    long series = 0;
    std::string script;                  // def.id
    Params params;
    std::shared_ptr<IndOutput> out;
    std::vector<std::string> log;        // print() output and load errors
};

class IndWorker {
public:
    ~IndWorker();
    // Queues a job, replacing one still queued for the same indicator (only the latest series matters).
    void submit(IndJob job);
    std::vector<IndDone> take();         // finished jobs, oldest first
    bool busy();                         // queued or running jobs
    std::function<void()> on_done;       // called on the worker thread after each job

private:
    void run();
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<IndJob> queue_;
    std::vector<IndDone> done_;
    std::thread thread_;
    bool stop_ = false, running_ = false;
    // worker thread only
    lua_State* L_ = nullptr;
    int L_gen_ = -1;                     // scripts generation of the definitions loaded in L_
    std::vector<std::string> log_;       // L_'s print() sink
    std::vector<ScriptDef> loaded_;      // definitions loaded in L_ (by chunk name)
};
