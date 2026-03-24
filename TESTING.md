# Testing Caesium with chrony

Long-term evaluation of the Caesium NTP server using chrony on a Linux client (Debian/Ubuntu). This guide sets up Caesium as a monitored-only source alongside public NTP servers, collects data over days/weeks, and generates a comparison report.

## 1. Install chrony

```bash
sudo apt install chrony
```

## 2. Configure chrony

Edit `/etc/chrony/chrony.conf`. Add Caesium as a `noselect` source (monitored but never used for clock discipline), alongside your real NTP sources:

```conf
# Real NTP sources (used for clock discipline)
pool ntp.ubuntu.com iburst maxsources 4
server time.nist.gov iburst

# Caesium GPS server (monitor only — not used for clock discipline)
server CAESIUM_IP iburst noselect minpoll 4 maxpoll 6

# Enable logging
logdir /var/log/chrony
log rawmeasurements statistics tracking
```

Key options:
- **`noselect`** — chrony polls the server and logs data but never uses it to set the clock
- **`rawmeasurements`** — logs every NTP packet (use instead of `measurements` so `noselect` sources are captured)
- **`minpoll 4 maxpoll 6`** — poll every 16-64 seconds (more data points than the default)

Restart chrony:

```bash
sudo systemctl restart chrony
```

Verify the source is being polled:

```bash
chronyc sources -v
```

You should see Caesium listed with `?` (noselect) and the public sources with `*` or `+`.

## 3. Let it run

Collect data for at least 24 hours. A week gives a much better picture — you'll see diurnal temperature effects on crystal drift and variations in network conditions.

Check that logs are growing:

```bash
ls -la /var/log/chrony/
```

## 4. Analyze

Run the analysis script against the chrony logs:

```bash
python3 analyze_chrony.py /var/log/chrony/
```

This parses `measurements.log` and `statistics.log`, compares Caesium against the other NTP sources, and reports:
- Offset (mean, median, stddev, percentiles)
- Round-trip delay (mean, min, max, stddev)
- Jitter from regression statistics
- Frequency drift estimate
- Time series summary by hour/day

### Example output

```
Caesium NTP Source Analysis
==========================

Source: CAESIUM_IP (GPS, Stratum 1)
  Period:       2026-03-24 to 2026-03-31 (7 days)
  Samples:      10,847

  Offset:
    Mean:       +0.412 ms
    Median:     +0.389 ms
    Stddev:      0.287 ms
    P5/P95:     +0.021 ms / +0.891 ms

  Delay:
    Mean:        1.823 ms
    Min:         1.201 ms
    Stddev:      0.341 ms

  Jitter (from statistics.log):
    Mean stddev: 0.264 ms

Comparison: 132.163.96.3 (NIST, Stratum 2)
  Offset mean:  -0.465 ms
  Delay mean:    7.015 ms
  Jitter mean:   0.891 ms
```

## Log file formats

### measurements.log

| Column | Field | Key? |
|--------|-------|------|
| 1-2 | Date Time (UTC) | |
| 3 | Source IP | |
| 4 | Leap (N/+/-/?) | |
| 5 | Stratum | |
| 12 | **Offset (seconds)** | Primary metric |
| 13 | **Peer delay (seconds)** | Network quality |
| 14 | **Peer dispersion (seconds)** | Scatter |

### statistics.log

| Column | Field | Key? |
|--------|-------|------|
| 1-2 | Date Time (UTC) | |
| 3 | Source IP | |
| 4 | **Std dev of measurements** | Jitter |
| 5 | **Estimated offset** | Smoothed offset |
| 7 | **Frequency offset (s/s)** | Drift rate |
