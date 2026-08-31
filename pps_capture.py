#!/usr/bin/env python3
"""
Capture Caesium's PPS with a Saleae logic analyzer and characterise it.

Two modes:

  --mode pps      One channel: the GPS PPS. Reports period stability, jitter,
                  pulse width and dropped pulses. Measures the *analyzer's*
                  crystal error as a by-product, since a GPS-disciplined PPS
                  is the more accurate of the two clocks by orders of magnitude.

  --mode timebase Two channels: PPS plus the GPIO33 validation pulse emitted by
                  a firmware built with -D TIMEBASE_PULSE. Reports E_c, the
                  error between Caesium's *served* second and its own PPS.

                      E_c = t_pulse - t_PPS(nearest)

                  Positive E_c => the served second is long => Caesium serves
                  time that runs SLOW by that amount. See TIMEBASE.md.

Both modes measure only intervals between edges that Caesium itself generates,
so the host clock never enters the result. That is deliberate: the analyzer's
absolute timestamps are software-applied at capture start and scatter by tens
of milliseconds, which is useless at this scale.

Examples:
    python3 pps_capture.py --mode pps --duration 300
    python3 pps_capture.py --mode timebase --duration 600 --json out.json
"""

import argparse
import csv
import json
import os
import statistics
import sys
import tempfile

import saleae_mcp

# The PPS is 100 ms wide and the validation pulse 50 us, so a 5 us glitch
# filter rejects interference picked up on the probe without touching either.
DEFAULT_GLITCH_US = 5.0


def capture(duration, channels, sample_rate, glitch_us, outdir, save_sal=None):
    """Run one timed capture and return {channel: [(time, value), ...]}."""
    saleae_mcp.init()
    cfg = {
        "logicChannels": {"digitalChannels": list(channels)},
        "digitalSampleRate": sample_rate,
    }
    if glitch_us:
        cfg["glitchFilters"] = [{"channelIndex": c,
                                 "pulseWidthSeconds": glitch_us * 1e-6}
                                for c in channels]
    started = saleae_mcp.tool_json("start_capture", {
        "logicDeviceConfiguration": cfg,
        "captureConfiguration": {
            "bufferSizeMegabytes": 8000,
            "timedCaptureMode": {"durationSeconds": duration}},
    })
    cid = started["captureId"]
    try:
        saleae_mcp.tool("wait_capture", {"captureId": cid})
        saleae_mcp.tool("export_raw_data_csv", {
            "captureId": cid, "directory": outdir, "analogDownsampleRatio": 1})
        if save_sal:
            # NOTE: the parameter is 'filepath', not 'filePath'.
            saleae_mcp.tool("save_capture", {"captureId": cid,
                                             "filepath": save_sal})
    finally:
        saleae_mcp.tool("close_capture", {"captureId": cid})

    path = os.path.join(outdir, "digital.csv")
    with open(path) as fh:
        reader = csv.reader(fh)
        header = next(reader)
        # Header is "Time [s], Channel N, Channel M, ..."
        cols = [int(h.split()[-1]) for h in header[1:]]
        series = {c: [] for c in cols}
        for row in reader:
            if not row:
                continue
            t = float(row[0])
            for c, v in zip(cols, row[1:]):
                series[c].append((t, int(v)))
    return series


def edges(samples):
    """Return (rising, falling) edge times from a transition list."""
    rise, fall = [], []
    for i, (t, v) in enumerate(samples):
        if i == 0:
            continue
        if v == 1 and samples[i - 1][1] == 0:
            rise.append(t)
        elif v == 0 and samples[i - 1][1] == 1:
            fall.append(t)
    return rise, fall


def select_pps_edges(rise, fall, width_ms, tol_ms):
    """
    Keep only rising edges that begin a pulse of the expected PPS width.

    Interference picked up on the probe produces edges of essentially any
    width except the configured one, so matching on pulse width identifies the
    real PPS far more reliably than a glitch filter alone. Returns the matched
    (rising, falling) pairs.
    """
    lo, hi = (width_ms - tol_ms) / 1e3, (width_ms + tol_ms) / 1e3
    keep_r, keep_f = [], []
    j = 0
    for tr in rise:
        while j < len(fall) and fall[j] <= tr:
            j += 1
        if j < len(fall) and lo <= fall[j] - tr <= hi:
            keep_r.append(tr)
            keep_f.append(fall[j])
    return keep_r, keep_f


def analyse_pps(rise, fall, sample_rate):
    """Period, jitter, width and continuity statistics for the PPS."""
    if len(rise) < 3:
        raise SystemExit(f"only {len(rise)} PPS edges captured — check wiring")
    per = [rise[i + 1] - rise[i] for i in range(len(rise) - 1)]
    mean = statistics.fmean(per)
    dropped = sum(1 for p in per if abs(p - mean) > 0.5)

    # A least-squares line through (index, time) removes the constant frequency
    # difference between the two clocks; what is left is wander, dominated by
    # the analyzer's uncompensated crystal.
    k = list(range(len(rise)))
    kb, tb = statistics.fmean(k), statistics.fmean(rise)
    slope = (sum((x - kb) * (y - tb) for x, y in zip(k, rise))
             / sum((x - kb) ** 2 for x in k))
    resid = [(y - (tb + slope * (x - kb))) * 1e9 for x, y in zip(k, rise)]

    out = {
        "edges": len(rise),
        "period_mean_s": mean,
        "period_sd_ns": statistics.pstdev(per) * 1e9,
        "period_pp_ns": (max(per) - min(per)) * 1e9,
        "analyzer_ppm": (mean - 1.0) * 1e6,
        "dropped_pulses": dropped,
        "wander_sd_ns": statistics.pstdev(resid),
        "wander_pp_ns": max(resid) - min(resid),
        "quantization_ns": 1e9 / sample_rate,
    }
    widths = [f - r for r, f in zip(rise, fall)]
    if widths:
        out["width_mean_ms"] = statistics.fmean(widths) * 1e3
        out["width_sd_ns"] = statistics.pstdev(widths) * 1e9
    return out


def analyse_timebase(pps_rise, pulse_rise):
    """E_c for each validation pulse: signed distance to the nearest PPS edge."""
    if not pulse_rise:
        raise SystemExit(
            "no pulses on the validation channel — is the firmware built with "
            "-D TIMEBASE_PULSE and GPIO33 wired to the analyzer?")
    deltas = []
    i = 0
    for tp in pulse_rise:
        while i + 1 < len(pps_rise) and abs(pps_rise[i + 1] - tp) < abs(pps_rise[i] - tp):
            i += 1
        deltas.append((tp - pps_rise[i]) * 1e6)  # microseconds
    return {
        "pulses": len(deltas),
        "E_c_median_us": statistics.median(deltas),
        "E_c_mean_us": statistics.fmean(deltas),
        "E_c_sd_us": statistics.pstdev(deltas) if len(deltas) > 1 else 0.0,
        "E_c_min_us": min(deltas),
        "E_c_max_us": max(deltas),
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["pps", "timebase"], default="pps")
    ap.add_argument("--duration", type=float, default=300.0,
                    help="capture length in seconds (default 300)")
    ap.add_argument("--pps-channel", type=int, default=1)
    ap.add_argument("--pulse-channel", type=int, default=0,
                    help="GPIO33 validation pulse channel (timebase mode)")
    ap.add_argument("--sample-rate", type=int, default=100_000_000)
    ap.add_argument("--glitch-us", type=float, default=DEFAULT_GLITCH_US,
                    help=f"glitch filter width (default {DEFAULT_GLITCH_US}), 0 to disable")
    ap.add_argument("--pps-width-ms", type=float, default=100.0,
                    help="expected PPS pulse width (u-blox default 100 ms)")
    ap.add_argument("--width-tol-ms", type=float, default=1.0,
                    help="tolerance when matching the PPS pulse width")
    ap.add_argument("--outdir", help="where to write CSV (default: temp dir)")
    ap.add_argument("--save-sal", help="also save the raw .sal capture here")
    ap.add_argument("--json", help="write results as JSON to this path")
    a = ap.parse_args()

    channels = [a.pps_channel]
    if a.mode == "timebase":
        channels.insert(0, a.pulse_channel)

    outdir = a.outdir or tempfile.mkdtemp(prefix="caesium-pps-")
    os.makedirs(outdir, exist_ok=True)
    print(f"# capturing {a.duration:.0f}s on channels {channels} "
          f"at {a.sample_rate/1e6:.0f} MS/s (glitch filter {a.glitch_us} us)")
    series = capture(a.duration, channels, a.sample_rate, a.glitch_us,
                     outdir, a.save_sal)

    raw_rise, raw_fall = edges(series[a.pps_channel])
    pps_rise, pps_fall = select_pps_edges(raw_rise, raw_fall,
                                          a.pps_width_ms, a.width_tol_ms)
    rejected = len(raw_rise) - len(pps_rise)
    if rejected:
        print(f"# rejected {rejected} of {len(raw_rise)} rising edges on "
              f"ch{a.pps_channel} as interference (wrong pulse width)")
        if rejected > len(pps_rise):
            print("# WARNING: most edges were interference. Check the probe "
                  "ground lead — a floating return picks up switching noise.")
    results = {"mode": a.mode, "duration_s": a.duration,
               "interference_edges": rejected,
               "pps": analyse_pps(pps_rise, pps_fall, a.sample_rate)}

    p = results["pps"]
    print(f"\n--- PPS on channel {a.pps_channel} ---")
    print(f"edges              : {p['edges']} in {a.duration:.0f}s, "
          f"{p['dropped_pulses']} dropped")
    print(f"period mean        : {p['period_mean_s']:.9f} s")
    print(f"  -> analyzer clock: {p['analyzer_ppm']:+.3f} ppm "
          f"(PPS is the reference; this is the Saleae's error)")
    print(f"cycle-to-cycle sd  : {p['period_sd_ns']:.1f} ns "
          f"(quantization {p['quantization_ns']:.0f} ns)")
    print(f"wander over run    : {p['wander_pp_ns']/1000:.2f} us peak-to-peak")
    if "width_mean_ms" in p:
        print(f"pulse width        : {p['width_mean_ms']:.6f} ms "
              f"(sd {p['width_sd_ns']:.1f} ns)")

    if a.mode == "timebase":
        pulse_rise, _ = edges(series[a.pulse_channel])
        tb = analyse_timebase(pps_rise, pulse_rise)
        results["timebase"] = tb
        print(f"\n--- Time-base validation pulse on channel {a.pulse_channel} ---")
        print(f"pulses             : {tb['pulses']}")
        print(f"E_c median         : {tb['E_c_median_us']:+.3f} us")
        print(f"E_c mean / sd      : {tb['E_c_mean_us']:+.3f} / {tb['E_c_sd_us']:.3f} us")
        print(f"E_c range          : {tb['E_c_min_us']:+.3f} .. {tb['E_c_max_us']:+.3f} us")
        print("\npositive E_c => Caesium's served second is long => it serves time SLOW")

    if a.json:
        with open(a.json, "w") as fh:
            json.dump(results, fh, indent=1)
        print(f"\n# wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
