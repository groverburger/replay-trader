#include "indworker.h"

#include <algorithm>

IndWorker::~IndWorker() {
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void IndWorker::submit(IndJob job) {
    {
        std::lock_guard<std::mutex> l(mu_);
        auto same = [&](const IndJob& q) { return q.uid == job.uid; };
        queue_.erase(std::remove_if(queue_.begin(), queue_.end(), same), queue_.end());
        queue_.push_back(std::move(job));
        if (!thread_.joinable()) thread_ = std::thread(&IndWorker::run, this);
    }
    cv_.notify_one();
}

std::vector<IndDone> IndWorker::take() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<IndDone> d;
    d.swap(done_);
    return d;
}

bool IndWorker::busy() {
    std::lock_guard<std::mutex> l(mu_);
    return running_ || !queue_.empty();
}

void IndWorker::run() {
    for (;;) {
        IndJob job;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [&] { return stop_ || !queue_.empty(); });
            if (stop_) break;
            job = std::move(queue_.front());
            queue_.pop_front();
            running_ = true;
        }
        IndDone d;
        d.uid = job.uid, d.gen = job.gen, d.tf = job.tf, d.series = job.series, d.script = job.def.id;
        d.params = job.params;
        d.out = std::make_shared<IndOutput>();
        std::string err;
        if (L_ && L_gen_ != job.gen) {  // the scripts were reloaded: start from a clean state
            close_script_state(L_);
            L_ = nullptr, loaded_.clear();
        }
        if (!L_) {
            L_ = new_script_state(err);
            L_gen_ = job.gen;
            set_script_sink(L_, &log_);
        }
        const ScriptDef* def = nullptr;
        for (const ScriptDef& x : loaded_)
            if (x.chunk == job.def.chunk) def = &x;
        if (!def && err.empty()) {
            ScriptDef x = job.def;
            x.ref = -1, x.error.clear();
            load_script(L_, x);  // on failure x.error is set and run_indicator_in reports it
            loaded_.push_back(std::move(x));
            def = &loaded_.back();
        }
        if (!err.empty()) d.out->error = err;
        else run_indicator_in(L_, job.gen, *def, job.params, *job.s, job.tf, job.series, *d.out);
        d.log.swap(log_);
        job.s.reset();
        {
            std::lock_guard<std::mutex> l(mu_);
            done_.push_back(std::move(d));
            running_ = false;
        }
        if (on_done) on_done();
    }
    if (L_) close_script_state(L_);
    L_ = nullptr;
}
