#!/usr/bin/env python3
"""
NTP Stability / Stress Test for Caesium GPS Time Server

Queries the NTP server repeatedly and tracks offset + delay over time.
Reports statistics (mean, stddev, min, max, drift rate) to verify
the server remains stable under sustained load.

Usage:
    python3 test_ntp_stability.py [hostname] [--count N] [--interval S]

Example:
    python3 test_ntp_stability.py caesium.ubnt.local --count 60 --interval 1
"""

import socket
import struct
import sys
import time
import math

NTP_UNIX_DELTA = 2208988800
NTP_PORT = 123


def ntp_to_unix(ntp_time):
    seconds = (ntp_time >> 32) & 0xFFFFFFFF
    fraction = ntp_time & 0xFFFFFFFF
    return (seconds - NTP_UNIX_DELTA) + fraction / (2**32)


def unix_to_ntp(unix_time):
    seconds = int(unix_time) + NTP_UNIX_DELTA
    fraction = int((unix_time % 1) * (2**32))
    return (seconds << 32) | fraction


def query_ntp(host, addr, timeout=2.0):
    """Single NTP query. Returns (offset, delay) or raises on failure."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        li_vn_mode = (0 << 6) | (4 << 3) | 3
        t1 = time.time()
        transmit_ts = unix_to_ntp(t1)
        packet = struct.pack(
            "!B B B b I I 4s Q Q Q Q",
            li_vn_mode, 0, 0, 0, 0, 0,
            b'\x00\x00\x00\x00', 0, 0, 0, transmit_ts,
        )
        sock.sendto(packet, (addr, NTP_PORT))
        data, _ = sock.recvfrom(1024)
        t4 = time.time()

        if len(data) < 48:
            raise ValueError("Short packet")

        unpacked = struct.unpack("!B B B b I I 4s Q Q Q Q", data[:48])
        li = (unpacked[0] >> 6) & 0x3
        stratum = unpacked[1]
        t2 = ntp_to_unix(unpacked[9])
        t3 = ntp_to_unix(unpacked[10])

        delay = (t4 - t1) - (t3 - t2)
        offset = ((t2 - t1) + (t3 - t4)) / 2

        return offset, delay, stratum, li
    finally:
        sock.close()


def fmt_ms(val):
    return f"{val * 1000:+.3f} ms"


def main():
    host = "caesium.ubnt.local"
    count = 60
    interval = 1.0

    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] in ("--count", "-n") and i + 1 < len(args):
            count = int(args[i + 1])
            i += 2
        elif args[i] in ("--interval", "-i") and i + 1 < len(args):
            interval = float(args[i + 1])
            i += 2
        elif not args[i].startswith("-"):
            host = args[i]
            i += 1
        else:
            i += 1

    print(f"Caesium NTP Stability Test")
    print(f"Target:   {host}")
    print(f"Samples:  {count}")
    print(f"Interval: {interval}s")
    print()

    # Resolve once
    try:
        addr = socket.gethostbyname(host)
    except socket.gaierror as e:
        print(f"DNS resolution failed: {e}")
        sys.exit(1)

    print(f"Resolved: {addr}")
    print()
    print(f"{'#':>4}  {'Offset':>12}  {'Delay':>12}  {'Offset Δ':>12}  Status")
    print(f"{'─' * 4}  {'─' * 12}  {'─' * 12}  {'─' * 12}  {'─' * 20}")

    offsets = []
    delays = []
    times_rel = []
    errors = 0
    start_time = time.monotonic()

    for n in range(1, count + 1):
        try:
            offset, delay, stratum, li = query_ntp(host, addr)

            elapsed = time.monotonic() - start_time
            offsets.append(offset)
            delays.append(delay)
            times_rel.append(elapsed)

            # Delta from previous offset
            if len(offsets) > 1:
                delta = offset - offsets[-2]
                delta_str = fmt_ms(delta)
            else:
                delta_str = "      ---   "

            # Status
            if li == 3 or stratum == 0:
                status = "UNSYNC"
            elif abs(delay) > 0.050:
                status = "high delay"
            else:
                status = "ok"

            print(f"{n:4d}  {fmt_ms(offset):>12}  {fmt_ms(delay):>12}  {delta_str:>12}  {status}")

        except socket.timeout:
            errors += 1
            print(f"{n:4d}  {'TIMEOUT':>12}  {'---':>12}  {'---':>12}  timeout")
        except Exception as e:
            errors += 1
            print(f"{n:4d}  {'ERROR':>12}  {'---':>12}  {'---':>12}  {e}")

        if n < count:
            time.sleep(interval)

    # Summary
    print()
    print("=" * 70)
    print("SUMMARY")
    print("=" * 70)

    if not offsets:
        print("No successful samples!")
        sys.exit(1)

    n_ok = len(offsets)
    mean_offset = sum(offsets) / n_ok
    mean_delay = sum(delays) / n_ok

    if n_ok > 1:
        var_offset = sum((x - mean_offset) ** 2 for x in offsets) / (n_ok - 1)
        stddev_offset = math.sqrt(var_offset)
        var_delay = sum((x - mean_delay) ** 2 for x in delays) / (n_ok - 1)
        stddev_delay = math.sqrt(var_delay)

        # Linear regression for drift rate (offset vs elapsed time)
        t_mean = sum(times_rel) / n_ok
        o_mean = mean_offset
        num = sum((t - t_mean) * (o - o_mean) for t, o in zip(times_rel, offsets))
        den = sum((t - t_mean) ** 2 for t in times_rel)
        drift_rate = num / den if den > 0 else 0.0  # seconds per second
    else:
        stddev_offset = 0
        stddev_delay = 0
        drift_rate = 0

    total_time = times_rel[-1] if times_rel else 0

    print(f"  Samples:        {n_ok} ok, {errors} failed")
    print(f"  Duration:       {total_time:.1f}s")
    print()
    print(f"  Offset mean:    {fmt_ms(mean_offset)}")
    print(f"  Offset stddev:  {fmt_ms(stddev_offset)}")
    print(f"  Offset min:     {fmt_ms(min(offsets))}")
    print(f"  Offset max:     {fmt_ms(max(offsets))}")
    print(f"  Offset range:   {fmt_ms(max(offsets) - min(offsets))}")
    print()
    print(f"  Delay mean:     {fmt_ms(mean_delay)}")
    print(f"  Delay stddev:   {fmt_ms(stddev_delay)}")
    print(f"  Delay min:      {fmt_ms(min(delays))}")
    print(f"  Delay max:      {fmt_ms(max(delays))}")
    print()
    print(f"  Drift rate:     {drift_rate * 1e6:+.3f} µs/s ({drift_rate * 1e6 * 3600:+.1f} µs/hr)")
    print()

    # Verdict
    if stddev_offset * 1000 < 1:
        print("  ✅ Excellent stability — offset jitter < 1ms")
    elif stddev_offset * 1000 < 5:
        print("  ✅ Good stability — offset jitter < 5ms")
    elif stddev_offset * 1000 < 20:
        print("  ⚠️  Moderate jitter — offset stddev > 5ms")
    else:
        print("  ❌ High jitter — offset stddev > 20ms")

    if abs(drift_rate) < 1e-6:
        print("  ✅ No measurable drift")
    elif abs(drift_rate) < 10e-6:
        print(f"  ✅ Low drift ({drift_rate * 1e6:+.3f} µs/s)")
    else:
        print(f"  ⚠️  Drift detected ({drift_rate * 1e6:+.3f} µs/s)")


if __name__ == "__main__":
    main()
