#!/usr/bin/env python3
"""Look for any audio band whose power follows the key line in last-run.npz."""
import json
import sys

import numpy

RATE = 48000
run = numpy.load(sys.argv[1] if len(sys.argv) > 1 else "last-run.npz")
samples = run["samples"].astype(numpy.float64)
origin, downs, ups = int(run["origin"]), run["downs"], run["ups"]
hop, size = 48, 480                                  # 1 ms hop, 10 ms window
frames = (len(samples) - size) // hop
window = numpy.hanning(size)
power = numpy.empty((frames, size // 2 + 1))
for i in range(frames):
    power[i] = numpy.abs(numpy.fft.rfft(samples[i * hop:i * hop + size] * window)) ** 2
times = origin + ((numpy.arange(frames) * hop + size // 2) * 1_000_000_000 // RATE)
gate = numpy.zeros(frames)
for down, up in zip(downs, ups):
    gate[(times >= down) & (times < up)] = 1.0
span = (times > downs[0] - 200_000_000) & (times < ups[-1] + 200_000_000)
result = {"bands": []}
freqs = numpy.fft.rfftfreq(size, 1 / RATE)
total = power[:, (freqs >= 200) & (freqs <= 3000)].sum(axis=1)
series = {"200-3000 Hz total": total}
for low in range(300, 1100, 100):
    series[f"{low}-{low + 100} Hz"] = power[:, (freqs >= low) & (freqs < low + 100)].sum(axis=1)
for name, values in series.items():
    best = (0.0, 0)
    for lag in range(0, 120):                         # audio lags the key by 0..120 ms
        shifted = numpy.roll(gate, lag)
        r = numpy.corrcoef(values[span], shifted[span])[0, 1]
        if abs(r) > abs(best[0]):
            best = (float(r), lag)
    shifted = numpy.roll(gate, best[1])
    on = values[span][shifted[span] > 0.5].mean()
    off = values[span][shifted[span] < 0.5].mean()
    result["bands"].append({"band": name, "correlation": round(best[0], 3), "lag_ms": best[1],
                            "key_down_over_key_up_power": round(float(on / off), 2)})
print(json.dumps(result, indent=1))
