#include "datafeed.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "civil.h"
#include "http.h"

#ifdef _WIN32
#include <windows.h>
#endif

// One cached bar: US Central bar start (minutes since 1970, naive) and OHLCV as served.
struct RawBar {
    int32_t t;
    float o, h, l, c, v;
};
static_assert(sizeof(RawBar) == 24, "packed cache rows");

namespace {

const char* kHost = "api.karaiq.com";
const int kRowCap = 1380;   // the server's per-request row limit
const int kThreads = 4;     // the server queues requests per key; a few in flight hide the round trip
const int kFirstDay = static_cast<int>(days_from_civil(2009, 12, 1));
const char kMagic[8] = {'N', 'Q', 'R', 'A', 'W', '0', '0', '1'};

std::string month_name(int y, int m) {
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d", y, m);
    return b;
}

std::string day_name(int64_t day) {
    int y, m, d;
    civil_from_days(day, y, m, d);
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    return b;
}

fs::path cache_dir() { return data_dir() / "cache" / "NQ_1min"; }
fs::path month_file(int y, int m) { return cache_dir() / (month_name(y, m) + ".bin"); }

bool read_month(const fs::path& p, std::vector<RawBar>& out) {
    std::FILE* f = open_file(p, "rb");
    if (!f) return false;
    char magic[8];
    int64_t n = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, kMagic, 8) == 0 && std::fread(&n, 8, 1, f) == 1 &&
              n >= 0 && n < 100000;
    if (ok) {
        size_t at = out.size();
        out.resize(at + static_cast<size_t>(n));
        ok = std::fread(out.data() + at, sizeof(RawBar), static_cast<size_t>(n), f) == static_cast<size_t>(n);
        if (!ok) out.resize(at);
    }
    std::fclose(f);
    return ok;
}

bool write_month(const fs::path& p, const std::vector<RawBar>& rows) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    std::FILE* f = open_file(tmp, "wb");
    if (!f) return false;
    int64_t n = static_cast<int64_t>(rows.size());
    bool ok = std::fwrite(kMagic, 1, 8, f) == 8 && std::fwrite(&n, 8, 1, f) == 1 &&
              std::fwrite(rows.data(), sizeof(RawBar), rows.size(), f) == rows.size();
    ok = (std::fclose(f) == 0) && ok;
    if (ok) fs::rename(tmp, p, ec);  // replaces atomically, so a crash never leaves half a month
    return ok && !ec;
}

int64_t today_ct() {  // calendar day in Chicago; UTC-6 is close enough (the last week is always re-fetched)
    auto s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch());
    return floor_div(s.count() / 60 - 6 * 60, 1440);
}

// ---------------------------------------------------------------- JSON: [{"t":"...","o":1.5,...}, ...]
struct Json {
    const char* p;
    const char* e;
    void ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    bool eat(char c) {
        ws();
        if (p < e && *p == c) return ++p, true;
        return false;
    }
    bool str(std::string& out) {
        ws();
        if (p >= e || *p != '"') return false;
        out.clear();
        for (++p; p < e && *p != '"'; ++p) {
            if (*p == '\\' && p + 1 < e) ++p;
            out += *p;
        }
        return p < e && (++p, true);
    }
    bool num(double& out) {  // locale-independent (strtod follows the C locale's decimal point)
        ws();
        bool neg = p < e && *p == '-';
        if (neg) ++p;
        double v = 0;
        int digits = 0;
        for (; p < e && *p >= '0' && *p <= '9'; ++p, ++digits) v = v * 10 + (*p - '0');
        if (p < e && *p == '.')
            for (double s = 0.1; ++p < e && *p >= '0' && *p <= '9'; s *= 0.1, ++digits) v += (*p - '0') * s;
        if (p < e && (*p == 'e' || *p == 'E')) {
            ++p;
            bool eneg = p < e && *p == '-';
            if (p < e && (*p == '-' || *p == '+')) ++p;
            int x = 0;
            for (; p < e && *p >= '0' && *p <= '9'; ++p) x = x * 10 + (*p - '0');
            v *= std::pow(10.0, eneg ? -x : x);
        }
        out = neg ? -v : v;
        return digits > 0;
    }
    bool skip_value() {
        ws();
        std::string s;
        double d;
        if (p < e && *p == '"') return str(s);
        if (e - p >= 4 && (!std::strncmp(p, "null", 4) || !std::strncmp(p, "true", 4))) return p += 4, true;
        if (e - p >= 5 && !std::strncmp(p, "false", 5)) return p += 5, true;
        return num(d);
    }
};

bool parse_bars(const std::string& body, const std::string& day, std::vector<RawBar>& out) {
    Json j{body.data(), body.data() + body.size()};
    if (!j.eat('[')) return false;
    if (j.eat(']')) return true;
    std::string key, t;
    do {
        if (!j.eat('{')) return false;
        RawBar b{};
        bool have_t = false;
        if (!j.eat('}')) {
            do {
                if (!j.str(key) || !j.eat(':')) return false;
                double v = 0;
                if (key == "t") {
                    if (!j.str(t)) return false;
                    have_t = true;
                } else if (key.size() == 1 && std::strchr("ohlcv", key[0])) {
                    if (!j.num(v)) return false;
                    float f = static_cast<float>(v);
                    switch (key[0]) {
                        case 'o': b.o = f; break;
                        case 'h': b.h = f; break;
                        case 'l': b.l = f; break;
                        case 'c': b.c = f; break;
                        default: b.v = f;
                    }
                } else if (!j.skip_value()) {
                    return false;
                }
            } while (j.eat(','));
            if (!j.eat('}')) return false;
        }
        int64_t m;
        if (have_t && t.compare(0, 10, day) == 0 && parse_minutes(t.c_str(), m)) {  // the request's own day only
            b.t = static_cast<int32_t>(m);
            out.push_back(b);
        }
    } while (j.eat(','));
    return j.eat(']');
}

void sort_unique(std::vector<RawBar>& v) {
    std::stable_sort(v.begin(), v.end(), [](const RawBar& a, const RawBar& b) { return a.t < b.t; });
    v.erase(std::unique(v.begin(), v.end(), [](const RawBar& a, const RawBar& b) { return a.t == b.t; }), v.end());
}

// ---------------------------------------------------------------- contract rolls
// The API's continuous series splices to the next quarterly contract a few days before expiry. Each splice is a
// jump (new minus old contract) that no trader saw; the app shifts an open position by it instead
// of booking it as P&L. Minutes are New York bar starts. Through 2024-06 they are 00:00 on the 14th, with the
// spreads used by Team Safi's 15-minute research data; later ones were found on 1-minute bars (the largest
// open-minus-previous-close jump near expiry, which stands out by 10x or more).
struct Roll {
    const char* ny;  // first bar on the new contract
    double jump;     // new contract minus old, in points
};
const Roll kRolls[] = {
    {"2009-12-14 00:00", -2.75}, {"2010-06-14 00:00", -1.25}, {"2010-12-14 00:00", -1.0},
    {"2011-03-14 00:03", -2.25}, {"2011-06-14 00:00", -3.5}, {"2011-09-14 00:00", -4.25},
    {"2012-03-14 00:01", -3.75}, {"2012-06-14 00:00", -6.0}, {"2012-09-14 00:00", -6.75},
    {"2012-12-14 00:00", -4.5}, {"2013-03-14 00:01", -5.75}, {"2013-06-14 00:00", -5.75},
    {"2014-03-14 00:00", -6.75}, {"2015-09-14 00:00", -10.25}, {"2015-12-14 00:00", -6.75},
    {"2016-03-14 00:00", -8.75}, {"2016-06-14 00:00", -7.75}, {"2016-09-14 00:00", -4.5},
    {"2017-03-14 00:00", 3.25}, {"2017-06-14 00:00", 7.5}, {"2017-09-14 00:00", 6.75},
    {"2020-09-14 00:00", -11.25}, {"2020-12-14 00:00", 1.5}, {"2021-06-14 00:00", -9.0},
    {"2021-09-14 00:00", -7.25}, {"2022-03-14 00:00", -7.25}, {"2022-06-14 00:00", 29.0},
    {"2022-09-14 00:00", 80.25}, {"2022-12-14 00:00", 118.25}, {"2023-03-14 00:00", 125.25},
    {"2023-06-14 00:00", 183.5}, {"2023-09-14 00:00", 194.75}, {"2023-12-14 00:00", 212.5},
    {"2024-03-14 00:00", 250.75}, {"2024-06-14 00:00", 262.0}, {"2024-09-15 18:00", 229.5},
    {"2024-12-15 18:00", 301.5}, {"2025-03-14 00:00", 205.75}, {"2025-06-13 00:01", 211.75},
    {"2025-09-12 03:57", 229.75}, {"2025-12-12 03:07", 255.75}, {"2026-03-13 02:44", 213.0},
    {"2026-06-11 16:59", 280.5}, {"2026-09-11 10:19", 293.75},
};

struct RollAt {
    int64_t t;
    double jump;
};

std::vector<RollAt> known_rolls() {
    std::vector<RollAt> r;
    for (const Roll& k : kRolls) {
        int64_t t;
        parse_minutes(k.ny, t);
        r.push_back({t, k.jump});
    }
    return r;
}

// Quarters after the table: find the splice in the 8th..16th of the expiry month.
bool detect_roll(const Market& m, int y, int mo, double last_jump, RollAt& out) {
    int64_t a = days_from_civil(y, mo, 8) * 1440, b = days_from_civil(y, mo, 16) * 1440;
    auto lo = std::lower_bound(m.t.begin(), m.t.end(), a) - m.t.begin();
    auto hi = std::lower_bound(m.t.begin(), m.t.end(), b) - m.t.begin();
    if (hi - lo < 500 || static_cast<size_t>(hi) >= m.size()) return false;  // window not fully in the data yet
    double best = 0, second = 0;
    long at = -1;
    for (long i = std::max<long>(lo, 1); i < hi; ++i) {
        if (m.t[i] - m.t[i - 1] > 10) continue;  // ordinary session breaks and weekend gaps are not splices
        double j = m.o[i] - m.c[i - 1];
        if (j > best) second = std::max(second, best), best = j, at = i;
        else second = std::max(second, std::fabs(j));
    }
    double need = std::max(50.0, 0.5 * std::fabs(last_jump));
    if (at < 0 || best < need || best < 4 * second) return false;
    out = {m.t[at], best};
    return true;
}

void apply_rolls(Market& m, std::vector<std::string>* notes) {
    m.adj.assign(m.size(), 0.0f);
    if (m.size() == 0) return;
    std::vector<RollAt> rolls = known_rolls();
    int y, mo, d;
    civil_from_days(floor_div(rolls.back().t, 1440), y, mo, d);
    double last_jump = rolls.back().jump;
    int ly, lm, ld;
    civil_from_days(floor_div(m.t.back(), 1440), ly, lm, ld);
    for (int qy = y, qm = mo + 3;; qm += 3) {
        if (qm > 12) qm -= 12, ++qy;
        if (qy > ly || (qy == ly && qm > lm)) break;
        RollAt r;
        if (detect_roll(m, qy, qm, last_jump, r)) {
            rolls.push_back(r);
            last_jump = r.jump;
            if (notes) {
                char b[96];
                int ry, rm, rd;
                civil_from_days(floor_div(r.t, 1440), ry, rm, rd);
                std::snprintf(b, sizeof b, "roll %04d-%02d-%02d %02d:%02d NY detected, %+.2f pt", ry, rm, rd,
                              static_cast<int>(r.t % 1440 / 60), static_cast<int>(r.t % 60), r.jump);
                notes->push_back(b);
            }
        } else if (notes) {
            notes->push_back("no roll detected for " + month_name(qy, qm) + " (positions held across it show the jump)");
        }
    }
    // adj = traded price minus back-adjusted price: 0 on the newest contract, and before each splice the bars
    // were on an older contract, priced `jump` below the back-adjusted series.
    for (const RollAt& r : rolls) {
        size_t i = static_cast<size_t>(std::lower_bound(m.t.begin(), m.t.end(), r.t) - m.t.begin());
        for (size_t k = 0; k < i && k < m.size(); ++k) m.adj[k] -= static_cast<float>(r.jump);
    }
}

}  // namespace

// ---------------------------------------------------------------- files and keys
fs::path data_dir() {
    fs::path p;
#ifdef _WIN32
    if (const wchar_t* d = _wgetenv(L"NQRT_DATA_DIR")) p = d;
    else if (const wchar_t* a = _wgetenv(L"APPDATA")) p = fs::path(a) / "NQ Replay Trader";
    else p = fs::current_path() / "NQ Replay Trader";
#else
    const char* home = std::getenv("HOME");
    if (const char* d = std::getenv("NQRT_DATA_DIR")) p = d;
#ifdef __APPLE__
    else p = fs::path(home ? home : ".") / "Library" / "Application Support" / "NQ Replay Trader";
#else
    else if (const char* x = std::getenv("XDG_DATA_HOME")) p = fs::path(x) / "nq-replay-trader";
    else p = fs::path(home ? home : ".") / ".local" / "share" / "nq-replay-trader";
#endif
#endif
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

std::FILE* open_file(const fs::path& p, const char* mode) {
#ifdef _WIN32
    wchar_t wm[8] = {};
    for (int i = 0; mode[i] && i < 7; ++i) wm[i] = static_cast<wchar_t>(mode[i]);
    return _wfopen(p.c_str(), wm);
#else
    return std::fopen(p.c_str(), mode);
#endif
}

static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

static fs::path key_file() { return data_dir() / "api_key.txt"; }

std::string resolve_api_key(const char* cli, std::string& source) {
    if (cli && *cli) return source = "--api-key", trim(cli);
    for (const char* env : {"NQ_API_KEY", "KARAIQ_API_KEY"})
        if (const char* v = std::getenv(env); v && *v) return source = env, trim(v);
    std::string key;
    if (std::FILE* f = open_file(key_file(), "rb")) {
        char buf[512];
        size_t n = std::fread(buf, 1, sizeof buf, f);
        std::fclose(f);
        key = trim(std::string(buf, n));
    }
    source = key.empty() ? "" : "saved";
    return key;
}

bool save_api_key(const std::string& key) {
    fs::path p = key_file();
    std::FILE* f = open_file(p, "wb");
    if (!f) return false;
    std::string k = trim(key);
    bool ok = std::fwrite(k.data(), 1, k.size(), f) == k.size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, ec);  // POSIX: 0600
    return ok;
}

void forget_api_key() {
    std::error_code ec;
    fs::remove(key_file(), ec);
}

// ---------------------------------------------------------------- cache -> market
static std::vector<fs::path> month_files() {
    std::vector<fs::path> v;
    std::error_code ec;
    for (auto it = fs::directory_iterator(cache_dir(), ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().extension() == ".bin") v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

CacheSummary cache_summary() {
    CacheSummary s;
    std::vector<fs::path> files = month_files();
    for (const fs::path& p : files) {  // the bar count from each header
        std::FILE* f = open_file(p, "rb");
        if (!f) continue;
        char magic[8];
        int64_t n = 0;
        if (std::fread(magic, 1, 8, f) == 8 && std::fread(&n, 8, 1, f) == 1 && n > 0) {
            RawBar b;
            if (!s.bars && std::fread(&b, sizeof b, 1, f) == 1) s.first = b.t + 60;
            if (std::fseek(f, 16 + (n - 1) * static_cast<long>(sizeof(RawBar)), SEEK_SET) == 0 &&
                std::fread(&b, sizeof b, 1, f) == 1)
                s.last = b.t + 60;
            s.bars += static_cast<size_t>(n);
        }
        std::fclose(f);
    }
    return s;
}

bool load_cached_market(Market& m, std::string& err, std::vector<std::string>* notes) {
    std::vector<RawBar> rows;
    rows.reserve(6500000);
    for (const fs::path& p : month_files())
        if (!read_month(p, rows)) {
            err = "bad cache file " + p.string() + " (delete it to download that month again)";
            return false;
        }
    sort_unique(rows);
    if (rows.empty()) {
        err = "no NQ data downloaded yet";
        return false;
    }
    size_t n = rows.size();
    for (auto* col : {&m.o, &m.h, &m.l, &m.c, &m.v}) col->resize(n);
    m.t.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const RawBar& b = rows[i];
        m.t[i] = b.t + 60;  // Central -> New York: always one hour ahead (both observe US DST)
        m.o[i] = b.o, m.h[i] = b.h, m.l[i] = b.l, m.c[i] = b.c, m.v[i] = b.v;
    }
    apply_rolls(m, notes);
    return true;
}

// ---------------------------------------------------------------- downloader
std::string Downloader::error() const {
    std::lock_guard<std::mutex> l(mu_);
    return error_;
}
std::string Downloader::status() const {
    std::lock_guard<std::mutex> l(mu_);
    return status_;
}
void Downloader::set_error(const std::string& e) {
    std::lock_guard<std::mutex> l(mu_);
    if (error_.empty()) error_ = e;
}
void Downloader::set_status(const std::string& s) {
    std::lock_guard<std::mutex> l(mu_);
    status_ = s;
}

void Downloader::start(const std::string& key, int64_t from_day) {
    stop();
    {
        std::lock_guard<std::mutex> l(mu_);
        error_.clear(), status_ = "checking the cache";
    }
    done = 0, total = 0, cancel_ = false, finished_ = false, running_ = true;
    ++runs;
    th_ = std::thread(&Downloader::run, this, key, from_day);
}

void Downloader::cancel() { cancel_ = true; }

void Downloader::stop() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}

bool Downloader::fetch_day(Http& http, const std::string& key, int64_t day, std::vector<RawBar>& out,
                           std::string& err, bool& fatal) {
    std::string d = day_name(day), next = day_name(day + 1);
    std::string path = "/data/NQ?bar=1min&security_type=future&start=" + d + "&end=" + next +
                       "&limit=" + std::to_string(kRowCap);
    for (int attempt = 0; attempt < 6 && !cancel_; ++attempt) {
        HttpResult r = http.get(kHost, path, "X-API-Key: " + key);
        if (r.status == 200) {
            size_t at = out.size();
            if (parse_bars(r.body, d, out)) return true;
            out.resize(at);
            err = d + ": unexpected response from the API";
        } else if (r.status == 401 || r.status == 403) {
            fatal = true;
            err = "the API rejected the key (HTTP " + std::to_string(r.status) + ")";
            return false;
        } else if (r.status == 0) {
            err = "network error: " + r.error;
        } else {
            err = d + ": HTTP " + std::to_string(r.status) + " " + r.body.substr(0, 120);
            if (r.status >= 400 && r.status < 500 && r.status != 408 && r.status != 429) {
                fatal = true;
                return false;
            }
        }
        for (int k = 0; k < 30 * (attempt + 1) && !cancel_; ++k)  // back off 3, 6, 9... seconds
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (cancel_) err = "cancelled";
    return false;
}

void Downloader::run(std::string key, int64_t from_day) {
    const int64_t today = today_ct();
    const int64_t first = from_day >= 0 ? from_day : kFirstDay;
    // Re-fetch everything from the newest cached day (it may be partial) or the last week, whichever is earlier.
    CacheSummary cs = cache_summary();
    int64_t refetch = cs.bars ? std::min<int64_t>(floor_div(cs.last - 60, 1440), today - 7) : first;

    struct Month {
        int y, m;
        std::vector<int64_t> days;
    };
    std::vector<Month> plan;
    int y, mo, d;
    civil_from_days(first, y, mo, d);
    for (int64_t m0 = days_from_civil(y, mo, 1); m0 <= today;) {
        int ny = mo == 12 ? y + 1 : y, nm = mo == 12 ? 1 : mo + 1;
        int64_t m1 = days_from_civil(ny, nm, 1);
        bool have = fs::exists(month_file(y, mo));
        Month mon{y, mo, {}};
        for (int64_t dd = std::max(m0, first); dd < m1 && dd <= today; ++dd)
            if (weekday_of(dd) != 6 && (!have || dd >= refetch)) mon.days.push_back(dd);  // no Saturday session
        if (!mon.days.empty()) {
            total += static_cast<int>(mon.days.size());
            plan.push_back(std::move(mon));
        }
        m0 = m1, y = ny, mo = nm;
    }
    std::reverse(plan.begin(), plan.end());  // newest first
    for (const Month& mon : plan) {
        if (cancel_) break;
        set_status("downloading " + month_name(mon.y, mon.m));
        std::vector<std::vector<RawBar>> got(mon.days.size());
        std::vector<char> ok(mon.days.size(), 0);
        std::atomic<size_t> next{0};
        std::atomic<bool> stop_month{false};
        auto worker = [&] {
            Http http;
            for (size_t k; !cancel_ && !stop_month && (k = next++) < mon.days.size();) {
                std::string err;
                bool fatal = false;
                ok[k] = fetch_day(http, key, mon.days[k], got[k], err, fatal);
                if (!ok[k]) {
                    set_error(err);
                    stop_month = true;
                    if (fatal) cancel_ = true;
                }
                ++done;
                if (on_progress) on_progress();
            }
        };
        std::vector<std::thread> pool;
        for (int i = 0; i < std::min<int>(kThreads, static_cast<int>(mon.days.size())); ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        if (!error().empty() || cancel_) break;  // the month stays unwritten and is fetched again next time
        // Merge: keep cached rows on days we didn't fetch, replace the rest.
        std::vector<RawBar> rows;
        read_month(month_file(mon.y, mon.m), rows);
        std::set<int64_t> fetched(mon.days.begin(), mon.days.end());
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const RawBar& b) { return fetched.count(floor_div(b.t, 1440)) > 0; }),
                   rows.end());
        for (auto& g : got) rows.insert(rows.end(), g.begin(), g.end());
        sort_unique(rows);
        if (!write_month(month_file(mon.y, mon.m), rows)) {
            set_error("could not write " + month_file(mon.y, mon.m).string());
            break;
        }
    }
    if (cancel_ && error().empty()) set_error("cancelled");
    set_status(error().empty() ? "up to date" : "stopped");
    finished_ = true;
    running_ = false;
    if (on_progress) on_progress();
}
