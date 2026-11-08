// Chart indicators over a Series. Each uses only bars already revealed; the forming bar updates them live,
// as on a trading platform. Swing pivots are only marked once their right-hand bars have closed.
#pragma once

#include <cmath>
#include <vector>

#include "market.h"

namespace ind {

using Vec = std::vector<double>;
const double NaN = std::nan("");

inline Vec sma(const Vec& x, int n) {
    Vec out(x.size(), NaN);
    double s = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        s += x[i];
        if (i >= static_cast<size_t>(n)) s -= x[i - n];
        if (i + 1 >= static_cast<size_t>(n)) out[i] = s / n;
    }
    return out;
}

// EMA seeded with the SMA of the first n values.
inline Vec ema(const Vec& x, int n) {
    Vec out(x.size(), NaN);
    if (x.size() < static_cast<size_t>(n)) return out;
    double a = 2.0 / (n + 1), e = 0;
    for (int i = 0; i < n; ++i) e += x[i] / n;
    out[n - 1] = e;
    for (size_t i = n; i < x.size(); ++i) out[i] = e = a * x[i] + (1 - a) * e;
    return out;
}

inline void bollinger(const Vec& x, int n, double k, Vec& mid, Vec& up, Vec& lo) {
    mid = sma(x, n);
    up.assign(x.size(), NaN), lo.assign(x.size(), NaN);
    for (size_t i = n - 1; i < x.size() && n > 1; ++i) {
        double var = 0;
        for (size_t j = i + 1 - n; j <= i; ++j) var += (x[j] - mid[i]) * (x[j] - mid[i]);
        double sd = std::sqrt(var / n);
        up[i] = mid[i] + k * sd, lo[i] = mid[i] - k * sd;
    }
}

// Wilder's RSI.
inline Vec rsi(const Vec& x, int n) {
    Vec out(x.size(), NaN);
    if (x.size() <= static_cast<size_t>(n)) return out;
    double g = 0, l = 0;
    for (int i = 1; i <= n; ++i) {
        double d = x[i] - x[i - 1];
        (d > 0 ? g : l) += std::fabs(d) / n;
    }
    out[n] = l == 0 ? 100 : 100 - 100 / (1 + g / l);
    for (size_t i = n + 1; i < x.size(); ++i) {
        double d = x[i] - x[i - 1];
        g = (g * (n - 1) + std::max(d, 0.0)) / n;
        l = (l * (n - 1) + std::max(-d, 0.0)) / n;
        out[i] = l == 0 ? 100 : 100 - 100 / (1 + g / l);
    }
    return out;
}

inline void macd(const Vec& x, int fast, int slow, int signal, Vec& line, Vec& sig, Vec& hist) {
    Vec f = ema(x, fast), s = ema(x, slow);
    line.assign(x.size(), NaN);
    size_t start = x.size();
    for (size_t i = 0; i < x.size(); ++i)
        if (!std::isnan(f[i]) && !std::isnan(s[i])) {
            line[i] = f[i] - s[i];
            start = std::min(start, i);
        }
    sig.assign(x.size(), NaN), hist.assign(x.size(), NaN);
    if (start >= x.size()) return;
    Vec tail(line.begin() + start, line.end()), e = ema(tail, signal);
    for (size_t i = 0; i < e.size(); ++i) {
        sig[start + i] = e[i];
        if (!std::isnan(e[i])) hist[start + i] = line[start + i] - e[i];
    }
}

// Wilder's ATR.
inline Vec atr(const Series& s, int n) {
    Vec out(s.n(), NaN);
    double a = 0;
    for (size_t i = 0; i < s.n(); ++i) {
        double tr = i ? std::max({s.h[i] - s.l[i], std::fabs(s.h[i] - s.c[i - 1]), std::fabs(s.l[i] - s.c[i - 1])})
                      : s.h[i] - s.l[i];
        if (i < static_cast<size_t>(n)) {
            a += tr / n;
            if (i + 1 == static_cast<size_t>(n)) out[i] = a;
        } else {
            out[i] = a = (a * (n - 1) + tr) / n;
        }
    }
    return out;
}

// Session VWAP from each bar's typical price, reset at the 18:00 Globex open.
inline Vec vwap(const Series& s) {
    Vec out(s.n(), NaN);
    double pv = 0, vol = 0;
    long day = -1;
    for (size_t i = 0; i < s.n(); ++i) {
        long d = static_cast<long>(bucket_key(static_cast<int64_t>(s.t[i]), 1440));
        if (d != day) day = d, pv = 0, vol = 0;
        double tp = (s.h[i] + s.l[i] + s.c[i]) / 3;
        pv += tp * s.v[i], vol += s.v[i];
        out[i] = vol > 0 ? pv / vol : tp;
    }
    return out;
}

// Swing pivots: bar i is a pivot high if its high is above the `left` highs before it and at least the `right`
// highs after it (lows mirrored). Only bars whose right-hand bars have all closed (<= last_complete) qualify.
struct Pivot {
    size_t i;
    double price;
    bool high;
    long broken;  // first later bar that trades through the level, -1 if none yet
};

inline std::vector<Pivot> pivots(const Series& s, int left, int right) {
    std::vector<Pivot> out;
    long last = s.last_complete();
    for (long i = left; i + right <= last; ++i) {
        bool ph = true, pl = true;
        for (long j = i - left; j <= i + right && (ph || pl); ++j) {
            if (j == i) continue;
            if (j < i ? s.h[j] >= s.h[i] : s.h[j] > s.h[i]) ph = false;
            if (j < i ? s.l[j] <= s.l[i] : s.l[j] < s.l[i]) pl = false;
        }
        for (int k = 0; k < 2; ++k) {
            bool high = k == 0;
            if (!(high ? ph : pl)) continue;
            Pivot p{static_cast<size_t>(i), high ? s.h[i] : s.l[i], high, -1};
            for (size_t j = i + right + 1; j < s.n(); ++j)
                if (high ? s.h[j] > p.price : s.l[j] < p.price) { p.broken = static_cast<long>(j); break; }
            out.push_back(p);
        }
    }
    return out;
}

}  // namespace ind
