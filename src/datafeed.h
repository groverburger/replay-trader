// NQ 1-minute bars from the karaiq Futures Data API (https://api.karaiq.com), cached per month in the user's
// data folder.
//
// The API serves the continuous front month with US Central bar-start times, newest first, at most 1,380 rows
// per request (exactly one Globex day of minutes), so we request one calendar day at a time. Months already on
// disk are not fetched again, except the last week (and everything after the newest cached bar), which the API
// may still be filling in. The server answers one request at a time (~0.9 s each), so the full history takes
// about an hour; months are fetched newest first, so stopping early still leaves the most recent data.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "market.h"

namespace fs = std::filesystem;

// Per-user folder: macOS ~/Library/Application Support/Replay Trader, Windows %APPDATA%\Replay Trader,
// Linux ~/.local/share/replay-trader. NQRT_DATA_DIR overrides it. Created on first use.
fs::path data_dir();
std::FILE* open_file(const fs::path& p, const char* mode);  // UTF-8-safe on Windows

// --api-key, then NQ_API_KEY / KARAIQ_API_KEY, then the saved key. `source` says which one was used.
std::string resolve_api_key(const char* cli, std::string& source);
bool save_api_key(const std::string& key);
void forget_api_key();

struct CacheSummary {
    size_t bars = 0;
    int64_t first = 0, last = 0;  // bar starts, New York minutes
};
// What the cache holds, without building the market.
CacheSummary cache_summary();

// Builds the market from the cache: New York times, prices as traded, and the per-bar roll adjustment. `notes`
// gets a line for each roll found by detection (rather than from the built-in table).
bool load_cached_market(Market& m, std::string& err, std::vector<std::string>* notes = nullptr);

// A copy of `m` that rolls on QuantConnect's schedule instead of the data's: between the API's splice and
// QC's roll, prices are those of the contract QC still holds (shifted by the spread), and the position shift
// happens at QC's roll. For reproducing QuantConnect backtests.
Market qc_roll_schedule(const Market& m);

class Downloader {
public:
    ~Downloader() { stop(); }
    // Fetches every day the cache is missing, from `from_day` (days since 1970, CT) or 2009-12-01.
    void start(const std::string& key, int64_t from_day = -1);
    void stop();    // cancel and wait
    void cancel();  // ask to stop without waiting (finished() turns true soon after)
    bool running() const { return running_; }
    bool finished() const { return finished_; }
    std::string error() const;    // empty on success
    std::string status() const;   // e.g. "2014-03: 182/4,410 days"
    std::atomic<int> done{0}, total{0};
    int runs = 0;  // start() calls so far (UI thread)
    void (*on_progress)() = nullptr;  // called from worker threads (e.g. BK_window_wake)

private:
    void run(std::string key, int64_t from_day);
    bool fetch_day(class Http& http, const std::string& key, int64_t day, std::vector<struct RawBar>& out,
                   std::string& err, bool& fatal);
    void set_error(const std::string& e);
    void set_status(const std::string& s);
    std::thread th_;
    std::atomic<bool> running_{false}, finished_{false}, cancel_{false};
    mutable std::mutex mu_;
    std::string error_, status_;
};
