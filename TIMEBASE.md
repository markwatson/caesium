# Time-base validation — measuring the served second against the PPS

**Date:** 2026-08-31. Measuring host: `starship` (macOS, USB-C Ethernet `en9`,
`192.168.75.100/22`). Instrument: **Saleae Logic 8**, serial `9096B49FF5A5B2E4`,
driven through the Logic 2 MCP automation server.

Closes the open item in [HANDOFF.md](HANDOFF.md): *"the GPS time base was never
actually measured — the weakest link"*. See [session.md](session.md) for the
full investigation this follows from.

---

## The question

Caesium reads **0.81 ms behind** internet stratum-1 consensus. Two hypotheses
survive, and they are observationally identical from the network endpoints:

- **(a)** shared-segment WAN asymmetry of ≥1.12 ms upstream-slower
- **(b)** Caesium genuinely serving ~0.78 ms slow

Hypothesis (b) requires the served time to disagree with the device's own PPS,
because the device's packet-path contribution is capped at 0.25 ms by
`|bias| <= delta/2`. So the discriminating measurement is:

> Does the time Caesium **serves** agree with the PPS edge it **derives that
> time from**?

Call that error `E_c`.

---

## Why the obvious approach does not work

The tempting design is: timestamp the PPS edge with the analyzer, query NTP
from the host, compare. It cannot work, and it is worth recording why so nobody
retries it.

An NTP offset is an **epoch** quantity — "server clock minus my clock". The
analyzer measures **intervals**. Joining them requires converting Saleae time
into host time, and Logic 2's absolute (ISO8601) timestamps are applied in
software at capture start, so they carry USB and scheduling latency.

Measured directly — eight identical captures of the same free-running PPS,
recording where the rising edge lands in host-clock phase:

| Run | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| phase (s) | .888731 | .871371 | .886806 | .891237 | .987026 | .888778 | .889316 | .916521 |

**Spread 115.7 ms, sd 34.0 ms.** The PPS itself is stable to ~73 ns, so all of
that is the anchor. It is ~40x larger than the effect being chased. No amount
of averaging fixes it: the error is a constant per capture, not noise.

> The analyzer is an excellent interval instrument and a useless epoch
> instrument. Any design that needs it to know *what time it is* has already
> failed.

---

## The design that does work

Put **both** edges in Caesium's own clock domain, and measure only the interval
between them. The host clock then drops out of the measurement entirely.

Firmware built with `-D TIMEBASE_PULSE` (see `src/main.cpp`) emits a 50 us pulse
on **GPIO33**, one *served* second after the PPS the NTP path is currently
interpolating from:

```c
int64_t target = s.ppsTimeMicros + (int64_t)s.usPerPps;
```

`usPerPps` is the same calibrated interval `hwTimeToNtp()` uses, so if the
crystal calibration is wrong the pulse is wrong by exactly the amount the
served time is wrong. The next *real* PPS arrives one *true* second after that
reference edge. Therefore:

```
E_c = t_pulse - t_PPS(next)
```

> **Pulse landing after the PPS edge = Caesium serving slow by exactly that
> much.**

This also captures PPS ISR latency, which is the point: latency makes
`ppsTimeMicros` late, which makes the pulse late *and* the served time slow by
the same amount.

The measurement is immune to the analyzer's own limitations. Its crystal is
9.6 ppm off and wanders ~9 us over 5 minutes (below), but `E_c` is an interval
of about one second, over which that wander contributes ~33 ns.

### Wiring

| Signal | ESP32 pin | Analyzer channel |
|---|---|---|
| GPS PPS | GPIO16 | 1 |
| Validation pulse | GPIO33 | 0 |
| Ground | GND | probe ground (**short lead, see below**) |

GPIO33 is free on the EXT header and output-capable. GPIO34-39 are input-only.
GPIO17 is the Ethernet clock and is not led out.

### Running it

```bash
# Build and flash the debug firmware (default env is unaffected)
~/.platformio/penv/bin/pio run -e esp32-poe-iso-timebase -t upload

# Measure
python3 pps_capture.py --mode timebase --duration 600 --json timebase.json
```

### Decision tree

| Result | Meaning | Then |
|---|---|---|
| `E_c` within ~10 us | served time tracks PPS | `0.81 - 0.25 = ` **>=0.56 ms is WAN**. Investigation closed. |
| `E_c` ~ +800 us | the device time base is the culprit | WAN theory dead; debug PPS/epoch pairing and interpolation |
| in between | partial | fall back to the parked local GPS reference clock |

---

## Results so far

### PPS characterisation — 274 s at 100 MS/s, uninterrupted

| Quantity | Value |
|---|---|
| Rising edges | 274 in 274 s, **0 dropped, 0 extra** |
| Period mean | 0.999990377 s |
| Cycle-to-cycle sd | **73.1 ns** (10 ns quantization) |
| Period peak-to-peak | 330 ns |
| Pulse width | 99.999041 ms, sd **10.8 ns** |

The PPS is clean. No missing pulses, no width modulation, jitter bounded at
73 ns including the instrument's own contribution — consistent with the
NEO-M9N's ~30 ns RMS spec. This does not prove absolute accuracy (nothing
on hand can), but it eliminates the entire "PPS is glitching" class of
explanation for hypothesis (b).

### The analyzer's clock, as a by-product

Because a GPS-disciplined PPS is many orders of magnitude better than an
uncompensated crystal, this run measures the **Logic 8**, not Caesium:

- **-9.62 ppm** constant frequency error
- **9.0 us peak-to-peak wander** over 274 s, against 73 ns cycle-to-cycle

Record it as the correction factor for any long interval this instrument
reports. It is irrelevant to `E_c`, which is a one-second interval.

### NTP from this host

`python3 measure_ntp.py 192.168.71.54 -n 120`

| Quantity | Value |
|---|---|
| Stratum / LI | 1 / 0 |
| Round-trip floor | 1.053 ms |
| Server processing (T3-T2) | **24.0 us** |
| `|bias| <= delta/2` | 0.527 ms |

The 24.0 us independently reproduces serv1's 23.7 us on different hardware,
a different OS and a different NTP client. The separate T2/T3 stamps are
working as designed.

**This host is not the right one for the headline number.** Its round-trip
floor is 1.053 ms against serv1's 0.504 ms — the USB-C NIC costs ~0.55 ms and
it has no hardware timestamping. Keep serv1 for NTP; use this host for the
analyzer.

---

## Two hazards found while setting this up

**1. Tailscale captures the route to Caesium.** `192.168.72/22` is advertised
over `utun6`, so NTP traffic leaves through the tunnel: 2.9 ms round trip
versus 1.04 ms wired, with jitter roughly doubled. Any measurement taken with
the tunnel up is measuring Tailscale. `measure_ntp.py -I en9` pins the socket
to the wired interface; disabling Tailscale also works.

Note that Caesium is one router hop away regardless (TTL 63, no ARP entry) —
`.71.54` is outside this host's `/22`, matching serv1's routed topology.

**2. Probe interference — currently unresolved.** At t=275.86 s of the soak the
PPS line erupted into roughly 1.2 MHz of superimposed noise, ~60,000 edges per
second, and it is still present. The PPS survives intact underneath it (100.00 ms
pulses continuing at exactly 1 s spacing), so this is pickup on the probe, not
a device fault.

A 5 us glitch filter cuts it about 1000-fold but does not clear it, so
`pps_capture.py` identifies the PPS by matching its 100 ms pulse width, which is
robust to interference of any width but the right one. It reports how many edges
it rejected — if that number is large, fix the wiring rather than trusting the
result:

```
# rejected 4055 of 4109 rising edges on ch1 as interference (wrong pulse width)
```

**Before the timebase run, check the probe ground lead.** A long or floating
ground return is the usual cause. The 274 s of clean data above was captured
before the onset and is the trustworthy dataset.

---

## Status

- [x] Analyzer plumbing, PPS located on channel 1, capture tooling
- [x] Anchoring approach tested and ruled out, with numbers
- [x] PPS characterised: clean, no dropped pulses, 73 ns jitter
- [x] `-D TIMEBASE_PULSE` firmware written and **building** (44.2% flash, 8.2% RAM)
- [ ] Flash the debug build to Caesium — needs USB access, no OTA in firmware
- [ ] Wire GPIO33 to channel 0, fix the probe ground
- [ ] Run `--mode timebase` and resolve (a) vs (b)
