#!/usr/bin/env python3
"""
Chrony Log Analyzer for Caesium NTP Server Evaluation

Parses chrony measurement and statistics logs to compare Caesium
against other NTP sources over multi-day collection periods.

Usage:
    python3 analyze_chrony.py /var/log/chrony/
    python3 analyze_chrony.py /var/log/chrony/ --caesium-ip 192.168.71.54
"""

import math
import os
import sys
from collections import defaultdict
from datetime import datetime


def parse_measurements(path):
    """Parse measurements.log. Returns dict of {ip: [records]}."""
    sources = defaultdict(list)
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("=") or line.startswith("Date"):
                continue
            parts = line.split()
            if len(parts) < 17:
                continue
            try:
                dt = datetime.strptime(f"{parts[0]} {parts[1]}", "%Y-%m-%d %H:%M:%S")
                ip = parts[2]
                leap = parts[3]
                stratum = int(parts[4])
                offset = float(parts[11])
                delay = float(parts[12])
                dispersion = float(parts[13])
                sources[ip].append({
                    "dt": dt,
                    "leap": leap,
                    "stratum": stratum,
                    "offset": offset,
                    "delay": delay,
                    "dispersion": dispersion,
                })
            except (ValueError, IndexError):
                continue
    return sources


def parse_statistics(path):
    """Parse statistics.log. Returns dict of {ip: [records]}."""
    sources = defaultdict(list)
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("=") or line.startswith("Date"):
                continue
            parts = line.split()
            if len(parts) < 10:
                continue
            try:
                dt = datetime.strptime(f"{parts[0]} {parts[1]}", "%Y-%m-%d %H:%M:%S")
                ip = parts[2]
                stddev = float(parts[3])
                est_offset = float(parts[4])
                offset_stddev = float(parts[5])
                freq_offset = float(parts[6])
                num_samples = int(parts[9])
                sources[ip].append({
                    "dt": dt,
                    "stddev": stddev,
                    "est_offset": est_offset,
                    "offset_stddev": offset_stddev,
                    "freq_offset": freq_offset,
                    "num_samples": num_samples,
                })
            except (ValueError, IndexError):
                continue
    return sources


def percentile(data, p):
    """Compute the p-th percentile of a sorted list."""
    if not data:
        return 0
    k = (len(data) - 1) * p / 100
    f = int(k)
    c = f + 1 if f + 1 < len(data) else f
    d = k - f
    return data[f] + d * (data[c] - data[f])


def fmt_ms(val):
    """Format seconds as milliseconds string."""
    return f"{val * 1000:+.3f} ms"


def fmt_ms_unsigned(val):
    """Format seconds as unsigned milliseconds string."""
    return f"{val * 1000:.3f} ms"


def analyze_source(ip, measurements, statistics):
    """Analyze a single source and return a report dict."""
    report = {"ip": ip, "samples": len(measurements)}

    if not measurements:
        return report

    first = measurements[0]["dt"]
    last = measurements[-1]["dt"]
    report["first"] = first
    report["last"] = last
    report["days"] = (last - first).total_seconds() / 86400

    report["stratum"] = measurements[0]["stratum"]

    offsets = [m["offset"] for m in measurements]
    delays = [m["delay"] for m in measurements]
    dispersions = [m["dispersion"] for m in measurements]

    report["offset_mean"] = sum(offsets) / len(offsets)
    report["offset_median"] = percentile(sorted(offsets), 50)
    if len(offsets) > 1:
        mean = report["offset_mean"]
        report["offset_stddev"] = math.sqrt(
            sum((x - mean) ** 2 for x in offsets) / (len(offsets) - 1)
        )
    else:
        report["offset_stddev"] = 0

    sorted_offsets = sorted(offsets)
    report["offset_p5"] = percentile(sorted_offsets, 5)
    report["offset_p95"] = percentile(sorted_offsets, 95)

    report["delay_mean"] = sum(delays) / len(delays)
    report["delay_min"] = min(delays)
    report["delay_max"] = max(delays)
    if len(delays) > 1:
        mean = report["delay_mean"]
        report["delay_stddev"] = math.sqrt(
            sum((x - mean) ** 2 for x in delays) / (len(delays) - 1)
        )
    else:
        report["delay_stddev"] = 0

    # Leap status breakdown
    leap_counts = defaultdict(int)
    for m in measurements:
        leap_counts[m["leap"]] += 1
    report["leap_counts"] = dict(leap_counts)

    # Statistics (jitter from regression)
    if statistics:
        jitters = [s["stddev"] for s in statistics]
        freq_offsets = [s["freq_offset"] for s in statistics]
        report["jitter_mean"] = sum(jitters) / len(jitters)
        report["freq_offset_mean"] = sum(freq_offsets) / len(freq_offsets)
    else:
        report["jitter_mean"] = None
        report["freq_offset_mean"] = None

    # Hourly breakdown
    hourly = defaultdict(list)
    for m in measurements:
        hour_key = m["dt"].strftime("%Y-%m-%d %H:00")
        hourly[hour_key].append(m["offset"])

    report["hourly_offset"] = {
        k: sum(v) / len(v) for k, v in sorted(hourly.items())
    }

    return report


def print_report(report, is_caesium=False):
    """Print a formatted report for one source."""
    label = f"{report['ip']}"
    if is_caesium:
        label += " (Caesium GPS)"
    label += f", Stratum {report.get('stratum', '?')}"

    print(f"\n{'=' * 60}")
    print(f"  {label}")
    print(f"{'=' * 60}")

    if report["samples"] == 0:
        print("  No measurements found.")
        return

    days = report.get("days", 0)
    print(f"  Period:       {report['first']} to {report['last']}")
    print(f"                ({days:.1f} days, {report['samples']} samples)")

    leaps = report.get("leap_counts", {})
    if leaps:
        parts = [f"{k}={v}" for k, v in sorted(leaps.items())]
        print(f"  Leap status:  {', '.join(parts)}")

    print()
    print(f"  Offset:")
    print(f"    Mean:       {fmt_ms(report['offset_mean'])}")
    print(f"    Median:     {fmt_ms(report['offset_median'])}")
    print(f"    Stddev:     {fmt_ms_unsigned(report['offset_stddev'])}")
    print(f"    P5/P95:     {fmt_ms(report['offset_p5'])} / {fmt_ms(report['offset_p95'])}")

    print()
    print(f"  Delay:")
    print(f"    Mean:       {fmt_ms_unsigned(report['delay_mean'])}")
    print(f"    Min:        {fmt_ms_unsigned(report['delay_min'])}")
    print(f"    Max:        {fmt_ms_unsigned(report['delay_max'])}")
    print(f"    Stddev:     {fmt_ms_unsigned(report['delay_stddev'])}")

    if report.get("jitter_mean") is not None:
        print()
        print(f"  Jitter (from regression):")
        print(f"    Mean:       {fmt_ms_unsigned(report['jitter_mean'])}")

    if report.get("freq_offset_mean") is not None:
        ppm = report["freq_offset_mean"] * 1e6
        print(f"  Freq offset:  {ppm:+.3f} ppm")


def main():
    log_dir = "/var/log/chrony"
    caesium_ip = None

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--caesium-ip" and i + 1 < len(args):
            caesium_ip = args[i + 1]
            i += 2
        elif not args[i].startswith("-"):
            log_dir = args[i]
            i += 1
        else:
            i += 1

    meas_path = os.path.join(log_dir, "measurements.log")
    stats_path = os.path.join(log_dir, "statistics.log")

    if not os.path.exists(meas_path):
        print(f"Error: {meas_path} not found")
        print(f"Make sure chrony logging is enabled and the path is correct.")
        sys.exit(1)

    print("Caesium NTP Source Analysis")
    print("=" * 60)
    print(f"Log directory: {log_dir}")

    measurements = parse_measurements(meas_path)
    statistics = parse_statistics(stats_path) if os.path.exists(stats_path) else {}

    if not measurements:
        print("\nNo measurement data found.")
        sys.exit(1)

    print(f"Sources found: {', '.join(sorted(measurements.keys()))}")

    # Auto-detect Caesium: look for stratum 1 with GPS refid on the local network
    if caesium_ip is None:
        for ip in measurements:
            samples = measurements[ip]
            if samples and samples[0]["stratum"] == 1:
                # Heuristic: local network IP with stratum 1
                if ip.startswith("192.168.") or ip.startswith("10.") or ip.startswith("172."):
                    caesium_ip = ip
                    break

    if caesium_ip:
        print(f"Caesium:       {caesium_ip}")

    # Analyze each source
    reports = {}
    for ip in sorted(measurements.keys()):
        reports[ip] = analyze_source(ip, measurements[ip], statistics.get(ip, []))

    # Print Caesium first
    if caesium_ip and caesium_ip in reports:
        print_report(reports[caesium_ip], is_caesium=True)

    # Print other sources
    for ip in sorted(reports.keys()):
        if ip != caesium_ip:
            print_report(reports[ip], is_caesium=False)

    # Comparison summary
    if caesium_ip and caesium_ip in reports and len(reports) > 1:
        print(f"\n{'=' * 60}")
        print(f"  Comparison Summary")
        print(f"{'=' * 60}")
        print(f"  {'Source':<20} {'Offset':>12} {'Delay':>12} {'Jitter':>12}")
        print(f"  {'─' * 20} {'─' * 12} {'─' * 12} {'─' * 12}")
        for ip in sorted(reports.keys()):
            r = reports[ip]
            if r["samples"] == 0:
                continue
            name = ip
            if ip == caesium_ip:
                name += " *"
            jitter = fmt_ms_unsigned(r["jitter_mean"]) if r.get("jitter_mean") is not None else "n/a"
            print(f"  {name:<20} {fmt_ms(r['offset_mean']):>12} {fmt_ms_unsigned(r['delay_mean']):>12} {jitter:>12}")
        print(f"\n  * = Caesium (noselect, monitor only)")


if __name__ == "__main__":
    main()
