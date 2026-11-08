// 1-minute NQ bars and their aggregation into chart timeframes, revealed up to a replay cursor.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

constexpr double kTick = 0.25;

// Times are New York local, minutes since 1970-01-01 (naive). Prices are as traded (front month).
struct Market {
    std::vector<int32_t> t;           // bar start
    std::vector<float> o, h, l, c, v;
    std::vector<float> adj;           // raw minus back-adjusted price; changes only at contract rolls
    size_t size() const { return t.size(); }
    bool load(const std::string& path, std::string& err);
    // Index of the last bar starting before `minute` (or 0).
    size_t index_before(int64_t minute) const;
};

// Chart timeframe in minutes; 1440 = daily (Globex session, 18:00 to 17:00).
int64_t bucket_key(int64_t t, int tf);

// Complete aggregated bars of one timeframe over the whole data set (empty for tf 1: the market bars are used).
struct Agg {
    int tf = 0;
    std::vector<int64_t> first;       // index of the bucket's first 1-minute bar
    std::vector<int64_t> t;           // bucket start (first minute's time; key start for intraday)
    std::vector<double> o, h, l, c, v;
    void build(const Market& m, int tf);
    // Bucket containing 1-minute bar `i`.
    size_t bucket_of(size_t i) const;
};

// Chart bars from bucket `lo` to the bucket holding the cursor; the last one is built only from 1-minute
// bars up to the cursor (it is "partial" unless the cursor is the bucket's last minute).
struct Series {
    int tf = 1;
    int64_t x0 = 0;                   // absolute bucket index of element 0 (used as the chart x)
    bool last_partial = false;
    std::vector<double> t, o, h, l, c, v;
    size_t n() const { return c.size(); }
    // Last index whose bar is complete (n-1, or n-2 when the last bar is still forming); -1 if none.
    long last_complete() const { return static_cast<long>(n()) - 1 - (last_partial ? 1 : 0); }
};

void build_series(const Market& m, const Agg& a, size_t cursor, int64_t lo_bucket, Series& out);
