"""Export NQ 1-minute bars for the replay trader.

Source: a parquet file of NQ 1-minute bars (continuous front month, back-adjusted, with raw closes); set SRC.
Writes data/nq_1m.bin with prices as traded (unadjusted front month) plus, per bar, the back-adjustment
offset, so the app can roll an open position across a contract change without a fake P&L jump.

Layout (little endian): magic "NQ1M0001", int64 rows, then column arrays:
    int32 t_start      bar start, New York local time, minutes since 1970-01-01 (naive)
    float32 open, high, low, close, volume
    float32 adj        raw price minus back-adjusted price (changes only at rolls)

Usage: python3 tools/export_bars.py   (the app downloads its own data; this is only for --data)
"""

from pathlib import Path

import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent.parent  # the repo root
SRC = HERE / "data" / "nq_1min.parquet"  # columns: bar_start_ny, open, high, low, close, volume, raw_close


def main():
    m = pd.read_parquet(SRC, columns=["bar_start_ny", "open", "high", "low", "close", "volume", "raw_close"])
    adj = (m.raw_close - m.close).to_numpy()
    t = m.bar_start_ny.to_numpy().astype("datetime64[m]").astype(np.int64)
    assert t.max() < 2 ** 31 and np.all(np.diff(t) > 0)
    cols = [t.astype("<i4")] + [(m[k].to_numpy() + adj).astype("<f4") for k in ("open", "high", "low", "close")]
    cols += [m.volume.to_numpy().astype("<f4"), adj.astype("<f4")]
    out = HERE / "data" / "nq_1m.bin"
    out.parent.mkdir(exist_ok=True)
    with open(out, "wb") as f:
        f.write(b"NQ1M0001")
        f.write(np.array([len(t)], dtype="<i8").tobytes())
        for c in cols:
            f.write(c.tobytes())
    print(f"wrote {out} ({len(t):,} bars, {out.stat().st_size / 1e6:.0f} MB), "
          f"{m.bar_start_ny.iloc[0]} .. {m.bar_start_ny.iloc[-1]}")


if __name__ == "__main__":
    main()
