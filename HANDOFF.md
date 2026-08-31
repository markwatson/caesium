# Handoff — Caesium accuracy investigation

**Date:** 2026-08-30. Written to resume this work in a fresh session or on
another machine. Self-contained: all key numbers are inline, because the
`report_*.log` files are gitignored (`*.log`) and will NOT travel with the repo.

Related: [HWTIMESTAMPING.md](HWTIMESTAMPING.md) (design sketch, conditional on
the outcome below), [report_20260830.log](report_20260830.log) (full data,
local only), [TESTING.md](TESTING.md) (how the chrony soak works).

---

## The one open question

Caesium reads **0.81 ms behind** the internet stratum-1 consensus. That gap
**cannot be attributed from the endpoints** — two equations, three unknowns.
Two hypotheses fit the data identically:

- **(a)** Shared-segment WAN asymmetry of ≥1.12 ms upstream-slower.
- **(b)** Caesium genuinely running 0.78 ms slow.

Everything below exists to distinguish these two.

The device's contribution is capped at **0.25 ms** by `|bias| ≤ delta/2` — an
airtight inequality, since all six un-stamped legs are non-negative durations
and the measured round trip is 0.504 ms. So (b) has no available mechanism...
**provided** the GPS time base is faithful, which is the one thing never
actually measured. That assumption is the weakest link in the whole chain, and
closing it is the next action.

---

## Setup

| Thing | Value |
|---|---|
| Caesium | `192.168.71.54`, `caesium.ubnt.local`, ESP32-PoE-ISO + u-blox NEO-M9N |
| Measuring host | `serv1` — `192.168.75.3/22`, `eno1` (e1000e), chrony 4.2, Lenovo ThinkServer TS140 |
| Path | **Routed, not switched** — Caesium is outside the /22, via `192.168.72.1` |
| Client NIC | Hardware timestamping available and unused (`hwtimestamp eno1`) |

---

## Key numbers (28.8 days, 4119 samples, 2026-08-02 → 08-31)

| Quantity | Value |
|---|---|
| Caesium offset (self-referential — see gotcha 1) | median +0.001 ms, sd 0.103 ms |
| Independent stratum-1 sources vs local clock | +0.74 … +0.82 ms (NIST ×3, Google ×2) |
| NTP round-trip floor (kernel-stamped) | 0.504 ms |
| ICMP round-trip floor to same device | 0.407 ms |
| Stamped server processing (T3−T2) | 23.7 µs |
| Un-stamped ESP32 latency (`s_rx + s_tx`) | ~390 µs |
| `s_tx` lower bound (from NTP−ICMP gap) | ≥ ~100 µs |
| Estimated device bias `½(s_rx − s_tx)` | +95 µs (range −195 … +95) |
| Jitter (chrony regression) | 0.024 ms |
| Differential, April vs August | 0.97 ms → 0.81 ms (**stable**) |

---

## Ruled out (with evidence — do not re-litigate)

- **Thermal.** Offset by local hour varies 0.128 ms peak-to-peak over 28.8 days
  with no coherent daily cycle. Structurally impossible anyway: thermal drift is
  a frequency effect and PPS re-disciplines every second.
- **Interpolation-rate error.** 400 live queries swept across sub-second phase:
  offset flat, slope +15 µs per full second (~15 ppm residual). The EMA crystal
  calibration works.
- **Leap seconds.** LI=`N` on all 4119 samples. Leap errors are 1 s anyway.
- **Aging / drift.** Differential stable across 5 months, a config change, a
  reboot, and different server IPs.
- **The switch.** Queueing is non-negative in both directions and chrony's
  clock filter selects minimum-delay samples, so bursty gear adds variance that
  gets rejected — it cannot produce a systematic one-directional bias. The
  router forwards cleanly (mdev 0.041 ms through it).
- **The GPS receiver itself.** NEO-M9N PPS is spec'd ~30 ns RMS — four orders of
  magnitude below the effect.
- **IEEE 1588 as an option.** The original ESP32 EMAC has **no** timestamp unit.
  Espressif (esp-idf #13423): *"IEEE1588 is not supported by ESP32. The ESP32 TRM
  used to list it as supported feature in past but it was a mistake."*
- **The 310 ms excursion on 2026-08-20.** That was the measuring host rebooting.
  All sources saw +308…+312 ms simultaneously; Caesium was the tightest.

---

## NEXT ACTION: time-base validation pulse

Cheapest, sharpest, and needs no purchases beyond a ~$15 logic analyzer.
Measures `E_c` — whether the *served* time actually tracks the PPS edge.

Add a debug pulse fired from the same time base that serves NTP:

```c
#define TIMEBASE_PIN 33          // free on EXT header; NOT bootstrap/SD/input-only
                                 // avoid GPIO17 — Ethernet clock, not led out

// setup(): pinMode(TIMEBASE_PIN, OUTPUT);

// loop():
static uint32_t lastPulseSec = 0;
TimeState s;
getTimeStateAtomic(s);
if (s.valid && s.epochSec != lastPulseSec) {
  int64_t target = s.ppsTimeMicros + s.usPerPps;   // one *served* second after last PPS
  int64_t delta  = target - esp_timer_get_time();
  if (delta > 0 && delta < 2000) {                 // only arm when close
    lastPulseSec = s.epochSec;
    while (esp_timer_get_time() < target) { }      // busy-wait, sub-us
    digitalWrite(TIMEBASE_PIN, HIGH);
    esp_rom_delay_us(50);
    digitalWrite(TIMEBASE_PIN, LOW);
  }
}
```

**Probe:** GPIO16 (PPS) on ch1, GPIO33 on ch2. Trigger ch1 rising. Measure
edge-to-edge.

**Reading it:** the pulse fires one *served* second after the last PPS; the next
*real* PPS arrives one true second after it. ISR latency makes `ppsTimeMicros`
late, which makes the pulse late **and** the served time slow by the same amount.

> **Pulse landing after the PPS edge = Caesium running slow by exactly that much.**

Caveats: `digitalWrite` adds ~1 µs constant overhead (use direct register writes
on a pin <32 for sub-µs). Blocks `loop()` up to 2 ms/sec — harmless, NTP is
served from `tcpip_thread`, but keep it to a debug build.

### Decision tree

| Result | Meaning | Then |
|---|---|---|
| Pulse within ~10 µs of PPS | `E_c ≈ 0` confirmed | 0.81 − 0.25 = **≥0.56 ms is WAN**. Investigation closed. HWTIMESTAMPING.md becomes optional polish. |
| Pulse ~800 µs late | Device time base IS the culprit | WAN theory dead. Debug PPS↔epoch pairing and interpolation. |
| Something in between | Partial | Need the local reference clock (parked item below). |

---

## Parked

**Local GPS reference on the measuring host** (~$60–100) — only needed if the
pulse test is inconclusive. The TS140 enumerates a real 16550A at `0x3F8`
(`ttyS0`, idle, verified silent), `pps_ldisc`/`pps_gpio` modules present, chrony
has `+REFCLOCK`. Plan: GPS PPS → MAX3232 → DCD, `ldattach PPS /dev/ttyS0`,
`refclock PPS /dev/pps0 refid GPS`. Check first whether the port is brought out
to a rear DB9 or needs a COM-header bracket. Prefer a **timing-grade** module
(NEO-M8T / LEA-M8T) or a different chipset, for independence from the M9N.

**Shared-PPS between the two machines — ABANDONED.** Would buy ~30 ns of
common-mode cancellation against an 800,000 ns question, at the cost of a 5.5 m
run, ground-loop risk against the PoE-**ISO** isolation, and level shifting.
Not worth it. Also: never put a microcontroller in a PPS path — it *regenerates*
the edge with µs-scale variable latency. Buffer, never regenerate.

**`hwtimestamp eno1`** — one line, works today, sharpens all future measurement
by 10–20 µs and cuts jitter. Note the date when enabling; it shifts measured
delays and would otherwise look like a device change in the logs.

**`setDynamicModel(DYN_MODEL_STATIONARY)`** — not currently set anywhere in
`main.cpp`; the receiver runs the default portable model. Correct setting for a
fixed install. Sub-µs improvement, free.

---

## Gotchas that would silently ruin things

1. **`prefer` on the Caesium line makes the measurement circular.** `chrony.conf`
   currently has `server 192.168.71.54 iburst prefer`, so the system clock is
   disciplined TO Caesium and its measured offset is ~0 by construction. Remove
   `prefer` before using any local reference clock, or you will measure Caesium
   against itself again.
2. **A one-sided timestamping fix makes accuracy WORSE.** `s_rx` and `s_tx`
   currently point in opposite directions and partially cancel. Fixing TX alone
   takes the bias from ~+95 µs to ~+145 µs. See HWTIMESTAMPING.md. Fix the
   larger leg (`s_rx`) first, or both, never TX alone.
3. **Never calibrate against an internet reference.** Tuning a constant until
   the NIST comparison reads zero would bake ≥1.12 ms of ISP asymmetry into a
   device whose LAN clients never traverse that path.
4. **Google is not a usable reference.** `216.239.35.0` is anycast and its NTP
   path does not match its ICMP path: ICMP min 2.22 ms vs NTP peer delay
   13.78 ms. NIST is consistent by the same check (5.40 vs 6.83 ms). Effective
   independent references: **two organisations, not five sources.**

---

## Config changes already made (2026-08-30)

- `/etc/chrony/sources.d/caesium-ntp-server.sources` — removed the `noselect`
  Caesium entry (it duplicated the `chrony.conf` one and chrony was already
  discarding it). File is now empty; original saved as `.bak` alongside.
  `nist.sources` untouched. No chronyd restart was needed.
- Nothing else changed. No firmware modified.

## Repo files

| File | Purpose | Tracked? |
|---|---|---|
| `HANDOFF.md` | This file | yes |
| `HWTIMESTAMPING.md` | Design sketch, conditional on the pulse test | yes |
| `report_20260830.log` | Full 28.8-day analysis + notes | **no — `*.log` ignored** |
| `report_20260401.log` | April run, for comparison | **no — `*.log` ignored** |
| `analyze_chrony.py` | Log analyser. Only reads un-rotated logs — concatenate `.1`–`.4` first or you get ~18 h instead of 29 days | yes |

To resume on another machine, commit `HANDOFF.md` and `HWTIMESTAMPING.md`. The
reports will not travel; every number needed is reproduced above.
