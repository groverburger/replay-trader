#include "market.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

bool Market::load(const std::string& path, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open " + path; return false; }
    char magic[8];
    int64_t n = 0;
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, "NQ1M0001", 8) == 0 && std::fread(&n, 8, 1, f) == 1;
    if (ok) {
        t.resize(n);
        ok = std::fread(t.data(), 4, n, f) == static_cast<size_t>(n);
        for (auto* col : {&o, &h, &l, &c, &v, &adj}) {
            col->resize(n);
            ok = ok && std::fread(col->data(), 4, n, f) == static_cast<size_t>(n);
        }
    }
    std::fclose(f);
    if (!ok) err = path + ": bad or truncated file (re-run export_bars.py)";
    return ok;
}

size_t Market::index_before(int64_t minute) const {
    auto it = std::lower_bound(t.begin(), t.end(), minute);
    return it == t.begin() ? 0 : static_cast<size_t>(it - t.begin()) - 1;
}

int64_t bucket_key(int64_t t, int tf) {
    if (tf >= 1440) return (t + 6 * 60) / 1440;  // session day: 18:00 starts the next trading date
    return t - ((t % tf) + tf) % tf;
}

void Agg::build(const Market& m, int tf_) {
    tf = tf_;
    first.clear(), t.clear(), o.clear(), h.clear(), l.clear(), c.clear(), v.clear();
    if (tf == 1) return;  // 1-minute buckets are the market bars themselves; build_series reads them directly
    int64_t key = INT64_MIN;
    for (size_t i = 0; i < m.size(); ++i) {
        int64_t k = bucket_key(m.t[i], tf);
        if (k != key) {
            key = k;
            first.push_back(static_cast<int64_t>(i));
            t.push_back(tf >= 1440 ? m.t[i] : k);
            o.push_back(m.o[i]), h.push_back(m.h[i]), l.push_back(m.l[i]), c.push_back(m.c[i]), v.push_back(0);
        }
        h.back() = std::max<double>(h.back(), m.h[i]);
        l.back() = std::min<double>(l.back(), m.l[i]);
        c.back() = m.c[i];
        v.back() += m.v[i];
    }
}

size_t Agg::bucket_of(size_t i) const {
    if (tf == 1) return i;
    auto it = std::upper_bound(first.begin(), first.end(), static_cast<int64_t>(i));
    return static_cast<size_t>(it - first.begin()) - 1;
}

void build_series(const Market& m, const Agg& a, size_t cursor, int64_t lo_bucket, Series& out) {
    size_t b = a.bucket_of(cursor);
    size_t lo = static_cast<size_t>(std::clamp<int64_t>(lo_bucket, 0, static_cast<int64_t>(b)));
    out.tf = a.tf;
    out.x0 = static_cast<int64_t>(lo);
    size_t n = b - lo + 1;
    for (auto* col : {&out.t, &out.o, &out.h, &out.l, &out.c, &out.v}) col->resize(n);
    if (a.tf == 1) {
        for (size_t k = lo; k <= b; ++k) {
            size_t j = k - lo;
            out.t[j] = m.t[k], out.o[j] = m.o[k], out.h[j] = m.h[k], out.l[j] = m.l[k], out.c[j] = m.c[k];
            out.v[j] = m.v[k];
        }
        out.last_partial = false;
        return;
    }
    for (size_t k = lo; k < b; ++k) {
        size_t j = k - lo;
        out.t[j] = a.t[k], out.o[j] = a.o[k], out.h[j] = a.h[k], out.l[j] = a.l[k], out.c[j] = a.c[k], out.v[j] = a.v[k];
    }
    // the forming bar: only minutes up to the cursor
    size_t s = static_cast<size_t>(a.first[b]);
    double ho = m.o[s], hh = m.h[s], hl = m.l[s], hv = 0;
    for (size_t i = s; i <= cursor; ++i) hh = std::max<double>(hh, m.h[i]), hl = std::min<double>(hl, m.l[i]), hv += m.v[i];
    out.t[n - 1] = a.t[b], out.o[n - 1] = ho, out.h[n - 1] = hh, out.l[n - 1] = hl, out.c[n - 1] = m.c[cursor];
    out.v[n - 1] = hv;
    size_t bucket_end = b + 1 < a.first.size() ? static_cast<size_t>(a.first[b + 1]) : m.size();
    out.last_partial = cursor + 1 < bucket_end;
}
