# Time-base validation — measuring the served second against the PPS

**Date:** 2026-08-31. Measuring host: `starship` (macOS, USB-C Ethernet `en9`,
`192.168.75.100/22`). Instrument: **Saleae Logic 8**, serial `9096B49FF5A5B2E4`,
driven through the Logic 2 MCP automation server.

Closes the open item in [HANDOFF.md](HANDOFF.md): *"the GPS time base was never
actually measured — the weakest link"*. See [session.md](session.md) for the
full investigation this follows from.

---

## Result

**`E_c` = +5.07 µs** over 600 consecutive seconds. Caesium's served time tracks
its own PPS to five microseconds — **160x too small** to explain the 0.81 ms
gap. Hypothesis (b) is dead; the remaining **>=0.56 ms is WAN asymmetry**.

| | 60 s pilot | 600 s soak |
|---|---|---|
| Pulses | 60 | **600** |
| `E_c` median | +5.080 us | **+5.070 us** |
| `E_c` mean / sd | +5.151 / 0.327 us | **+5.122 / 0.246 us** |
| `E_c` range | +4.890 .. +7.340 us | +4.880 .. +8.930 us |

Two independent runs agree to **10 ns** on the median. See
[Results](#results-1) for the full budget.

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
| Validation pulse | **GPIO33 or GPIO32** | 0 |
| Ground | GND | probe ground (**short lead, see below**) |

The pulse is driven on **both GPIO32 and GPIO33 simultaneously** — same output
register bank, one atomic write, no skew. Probe whichever is easier: GPIO32 sits
directly adjacent to the PPS pin, GPIO33 two above it. This removes a fiddly
pin-counting step on a dense header where GPIO34-39 are input-only and a slip
onto one is indistinguishable from a bad contact.

GPIO17 is the Ethernet clock and is not led out. The PPS is also available on
the **GPS breakout's silkscreened PPS pin** — the same net as GPIO16, and a much
easier probe target.

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
| **`E_c` within ~10 us** | **served time tracks PPS** | **`0.81 - 0.25` = >=0.56 ms is WAN. Investigation closed.** |
| `E_c` ~ +800 us | the device time base is the culprit | WAN theory dead; debug PPS/epoch pairing and interpolation |
| in between | partial | fall back to the parked local GPS reference clock |

> **Outcome: row 1, at +5.07 us.** Investigation closed.

---

## Results

### Time-base validation — 600 s, 600 pulses

| Quantity | Value |
|---|---|
| Pulses emitted / captured | 600 / 600 |
| PPS edges | 600, **0 dropped** |
| **`E_c` median** | **+5.070 us** |
| `E_c` mean / sd | +5.122 / **0.246 us** |
| `E_c` range | +4.880 .. +8.930 us |
| PPS pulse width | 99.998941 ms, sd 20.3 ns |

**The sign is the consistency check.** `hwTimeToNtp()` anchors served time to
`ppsTimeMicros`, which the PPS ISR records *after* interrupt-entry latency L.
For any request, `elapsed = hwTime - ppsTimeMicros` is then short by L and the
served time comes out **slow** by L. The same L delays the pulse, which targets
`ppsTimeMicros + usPerPps`. So a faithful time base with real ISR latency
predicts a *positive* `E_c` — which is what HANDOFF.md said before any of this
was wired, and what was observed. A negative or scattered result would have
meant the rig was wrong, not the device.

+5 us is an entirely ordinary ESP32 GPIO interrupt latency, and sd 0.246 us
across 600 samples says it is a stable systematic, not noise.

### Under NTP load — 180 s at 4711 req/s

Run while flooding the device from six threads, to check whether serving
traffic perturbs the time base. It does, but read the caveat before drawing
conclusions.

| Quantity | Quiet | Under load |
|---|---|---|
| NTP queries | ~0 | **838,510 (4711 req/s)** |
| NTP RTT median / p99 | — | 1.259 / **1.406 ms** |
| Failed queries | — | **0** |
| `E_c` median | +5.070 us | **+22.900 us** |
| `E_c` mean / sd | +5.122 / 0.246 us | +42.537 / 43.740 us |
| `E_c` max | +8.930 us | **+189.490 us** |

**The robustness result is genuinely good.** Caesium sustained 4711 requests
per second with zero failures and a p99 round trip of 1.406 ms — barely above
its 1.05 ms floor. It does not fall over, drop requests, or lose GPS lock under
load far beyond anything a home network will produce.

**The `E_c` degradation is mostly instrument, not device.** This is important
and easy to get wrong. The two paths are not equally exposed to scheduling:

- The **pulse** is emitted from `loop()` on core 1. It busy-waits to `target`
  and then writes the GPIO — so any preemption of core 1 *at that instant*
  delays the edge. Under 4711 req/s of Ethernet and lwIP interrupt activity,
  preemption is frequent. The pulse is late; the time base need not be.
- The **NTP response** is timestamped inline in `tcpip_thread` with
  `esp_timer_get_time()` the moment the packet arrives. No `loop()` scheduling
  enters that path, and separate T2/T3 stamps make `s_proc` cancel regardless.

So this run bounds *pulse emission jitter under load*, and only indirectly says
anything about served accuracy. A component of it could be genuine — PPS ISR
latency may also rise under load, which would affect served time — but this
measurement cannot separate the two.

**It does not affect the WAN conclusion.** The 28.8-day chrony comparison ran at
normal poll intervals (a few queries per minute), which is the quiet regime. The
+5.07 us figure is the applicable one.

Separating device from instrument here would need the pulse emitted from an ISR
or a hardware timer rather than from `loop()`. Worth doing only if serving
accuracy under sustained heavy load ever becomes a real question.

### Post-fix verification — the load test redone properly

The load run above predated `b7fa5eb` and recorded only RTT, so it could not
have detected the epoch-labelling failure that commit guards against. Redone on
merged firmware, recording NTP **offset** concurrently — a 1 s epoch error shows
up there and nowhere else.

| | pre-fix quiet | pre-fix load | **post-fix quiet** | **post-fix load** |
|---|---|---|---|---|
| Requests/s | ~0 | 4711 | ~0 | **4258** |
| `E_c` median | +5.070 us | +22.900 us | **+5.120 us** | **+5.980 us** |
| `E_c` sd | 0.246 us | 43.740 us | **0.206 us** | 40.977 us |
| `E_c` max | +8.930 us | +189.490 us | +7.310 us | +127.670 us |

**The guard holds: 0 whole-second errors in 757,859 queries.** That is the
result the previous run could not produce, taken at the exact request rate that
provokes Core 1 starvation.

**Served accuracy does not degrade under load.** NTP offset held at median
+0.359 ms with **sd 0.084 ms** across 757,859 samples at 4258 req/s — *tighter*
than the quiet spot-checks taken earlier in the session. Two failures out of
757,859 (0.0003%). Delay floor 0.419 ms, better than the 1.05 ms idle figure.

**This confirms the caveat above: the `E_c` tail is instrument, not device.** If
the time base genuinely wandered by 128 us under load, the NTP offsets would
show it — 0.084 ms of spread across three quarters of a million samples says it
does not. Note also that the *median* barely moves (5.120 -> 5.980 us) while the
mean and max blow out: that is the signature of occasional preemption spikes on
an otherwise clean distribution, exactly as predicted for an edge emitted from
`loop()`.

The post-fix median under load (+5.980 us) is markedly better than pre-fix
(+22.900 us). Suggestive, but one run each — not a controlled comparison.

**Quiet `E_c` is unchanged by the fix** (+5.070 -> +5.120 us, sd 0.246 -> 0.206),
so the starvation guard costs nothing in the normal regime.

### The accuracy budget, now fully bounded

| Contribution | Bound | Source |
|---|---|---|
| Device packet path | <= 0.25 ms | `\|bias\| <= delta/2`, delta = 0.504 ms |
| Device time base (`E_c`) | **0.005 ms** | **measured here** |
| **Device total** | **<= 0.255 ms** | |
| Observed gap to internet stratum-1 | 0.81 ms | 28.8-day chrony soak |
| **Unaccounted -> WAN asymmetry** | **>= 0.56 ms** | by difference |

The time-base term was the only unmeasured quantity in that table and the one
HANDOFF.md called "the weakest link in the whole chain". It is now measured and
negligible.

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
- [x] Flash the debug build to Caesium
- [x] Wire the pulse to channel 0, fix the probe ground
- [x] Run `--mode timebase` — **resolved: (a) WAN asymmetry**
- [x] Load test at 4711 req/s — device robust; see caveat above
- [x] Raw captures archived in [data/](data/) — the rig was awkward to build

### What this does not prove

**`E_c` is blind to whole-second errors.** This is structural, not a limitation
of the run length. Commit `b7fa5eb` fixes a bug where PVT(n) could pair with
PPS(n+1)'s timestamp under Core 1 starvation, publishing a `timeState` exactly
1.000000 s in the past. Trace the pulse in that state: `target` becomes
`ppsTimeMicros(n+1) + usPerPps`, so it fires one second after PPS(n+1) and lands
on PPS(n+2). Nearest-edge matching then reports a *small* `E_c`, exactly as if
nothing were wrong.

So this measurement certifies the **sub-second phase** of the time base and says
nothing about **epoch labelling**. The two are complementary. It does not weaken
the conclusions here — the 0.81 ms gap is sub-second, and a 1 s error would have
been unmissable across 4119 chrony samples at sd 0.103 ms — but do not read
`E_c` as validating the whole time base.

Note also that the load test above ran on **pre-`b7fa5eb` firmware**, at exactly
the request rate that provokes the starvation path, and recorded only RTT rather
than offset. It therefore could not have detected that failure mode either.
Re-running the flood on merged firmware while recording NTP offset would close
this; the rig is described above.

The served time tracks the PPS. It does **not** independently verify the PPS
against UTC — that still rests on the NEO-M9N's ~30 ns spec. The assumption is
far better supported than before (0 dropped pulses across 874 s of capture,
14.5-184 ns jitter, pulse width stable to 9-20 ns), but it is still an
assumption. Closing it needs the parked independent reference clock.

The packet-path term likewise remains *bounded*, not measured. Measuring it is
what RFC 9769 interleaved mode plus RX timestamping would buy.

### Consequences

- **Caesium is better than 0.81 ms implied.** That number came from comparing
  through an asymmetric internet path. LAN clients are one router hop away and
  never traverse it; what they see is bounded by the ~0.25 ms packet term.
- **The asymmetry is not fixable from here.** It is in the shared upstream
  segment every traceroute crosses before diverging
  (`192.168.72.1 -> 207.225.112.10 -> 63.225.124.73`).
- **Gotcha 3 in HANDOFF.md is now load-bearing.** Tuning a constant until the
  NIST comparison reads zero would bake >=1.12 ms of ISP asymmetry into a device
  whose clients never see that path — making it materially worse for every real
  consumer while the graph looked perfect.
- **`HWTIMESTAMPING.md` is optional polish**, exactly as the decision tree said.
