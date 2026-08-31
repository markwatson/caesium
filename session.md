# Session record — 2026-08-30

Full narrative of the Caesium accuracy investigation: what was measured, what
was concluded, and — importantly — where reasoning was wrong and had to be
corrected. For "what to do next" in condensed form, see
[HANDOFF.md](HANDOFF.md). For the conditional firmware design, see
[HWTIMESTAMPING.md](HWTIMESTAMPING.md).

Machines: **serv1** (Lenovo ThinkServer TS140, `192.168.75.3/22`, `eno1`
e1000e, chrony 4.2) measuring **Caesium** (`192.168.71.54`,
`caesium.ubnt.local`, ESP32-PoE-ISO + u-blox NEO-M9N).

---

## Phase 1 — Locating the test setup

Question was where the skew testing lived and what the connection details were.

Found in-repo: `TESTING.md` (chrony soak procedure), `analyze_chrony.py`,
`test_ntp.py`, `test_ntp_stability.py`, `report_20260401.log` (a completed
3.9-day run). Nothing skew-related elsewhere in the home dir except a stale copy
of `test_ntp_stability.py` in `~/tmp`, one blank line off from the repo version.

Connection details: `caesium.ubnt.local` / `192.168.71.54`, port 123. Serial
console 115200; GPS UART 38400 UBX-only. Build is
`~/.platformio/penv/bin/pio run` per `AGENTS.md` — **but `~/.platformio` does
not exist on serv1**, so the project cannot be built here.

**Config contradiction found.** Caesium was configured twice, contradictorily:

- `/etc/chrony/chrony.conf` → `server 192.168.71.54 iburst prefer`
- `/etc/chrony/sources.d/caesium-ntp-server.sources` → `server caesium.ubnt.local minpoll 2 maxpoll 4 noselect`

`chronyc sources` showed a single `^*` entry, so chrony was already discarding
the duplicate. Resolved by removing the `noselect` entry (below).

---

## Phase 2 — The 29-day analysis

`analyze_chrony.py` only opens the un-rotated `measurements.log` /
`statistics.log`, so running it directly at `/var/log/chrony/` would have
yielded ~18 hours. Concatenated `.4 .3 .2 .1` + current into a scratch dir
first, giving **2026-08-02 → 08-31, 28.8 days, 4119 Caesium samples**.
Retention is 4 weekly rotations, so the April data is long gone.

Clean results, reboot window excluded:

| Source | Offset mean | sd | Delay |
|---|---|---|---|
| **Caesium (S1)** | +0.002 ms | 0.103 ms | 0.701 ms |
| NIST ×3 (S1) | +0.77 … +0.82 ms | ~0.3 ms | ~7 ms |
| Google ×2 (S1) | +0.74, +0.77 ms | ~0.4 ms | ~14 ms |
| Canonical ×4 (S2) | −1.74 … −0.16 ms | 1.0–3.1 ms | 45–114 ms |

Jitter 0.024 ms, freq offset +0.002 ppm, LI=`N` on all 4119 samples — no loss
of sync in four weeks. This closes the outstanding `TODO.md` item ("soak this
against a linux box for a week or so").

**Critical caveat.** Since 2026-03-30 `chrony.conf` carries `prefer`, so serv1's
clock is disciplined TO Caesium. Caesium's own measured offset is ~0 *by
construction* and is not evidence of accuracy. The real signal is the +0.81 ms
that independent stratum-1 servers show against the Caesium-disciplined clock.

**The 310 ms excursion.** Nine of 4119 samples exceed |1 ms|; three are one
event at 2026-08-20 23:18:56–23:19:01 UTC. `journalctl --list-boots` showed boot
−1 ending 17:16 MDT and boot 0 starting 17:18 MDT — **serv1 rebooted**. Every
source saw +308…+312 ms simultaneously and Caesium was the tightest of them.
Not a device fault.

---

## Phase 3 — Eliminating hypotheses

**Thermal — ruled out.** Mean offset by local hour over 28.8 days varies only
0.128 ms peak-to-peak, with no coherent daily cycle (min at 20–21h, maxima at
07h *and* 15–16h). Structurally impossible regardless: thermal drift is a
frequency effect and PPS re-disciplines every second.

**Interpolation-rate error — ruled out.** Wrote a live test sweeping 400 queries
uniformly across the sub-second phase. Offset flat vs phase, slope **+15 µs per
full second** (~15 ppm residual). The EMA crystal calibration in
`gps_time.cpp` provably works. Server processing (T3−T2) measured at **23.7 µs**.

**Leap seconds — ruled out.** LI=`N` on all samples; leap errors are 1 s.

**Aging / drift — ruled out.** See the April comparison in Phase 4.

**Code review.** `ntp.cpp` and `gps_time.cpp` read clean. `hwTimeToNtp` uses the
calibrated interval; interpolation error is bounded by ppm × elapsed, i.e.
microseconds. Two minor gaps found in `main.cpp`: no
`setDynamicModel(DYN_MODEL_STATIONARY)` (receiver runs the default portable
model despite being bolted to a shelf), and `userConfigDelay` is inherited from
the module rather than set. Both sub-µs; neither relevant to the question.

---

## Phase 4 — The asymmetry decomposition

Six un-stamped legs, none visible to NTP:

```
theta = E + ½[(c_tx + d_fwd + s_rx) − (s_tx + d_rev + c_rx)]
delta =       (c_tx + d_fwd + s_rx) + (s_tx + d_rev + c_rx)
```

So bias = ½(upstream − downstream) and **|bias| ≤ delta/2**, exactly. `s_proc`
cancels entirely — separate T2/T3 stamps buy that, so the 23.7 µs costs nothing.
Only the *difference* matters, never the magnitude.

Budget from chrony's kernel-stamped 0.504 ms floor: ~40–50 µs wire, ~10–20 µs
client legs, leaving **~390 µs as `s_rx + s_tx` inside the ESP32**. The
NTP-minus-ICMP gap (0.504 − 0.407) pins `s_tx ≥ ~100 µs`, because lwIP's
`icmp_input` builds its echo reply in the received pbuf while the NTP path does
`pbuf_alloc` + `memcpy` + `udp_sendto`.

**Therefore Caesium's own share is capped at ~0.25 ms** and cannot produce
0.81 ms. Expected sign works against it too: `s_rx` crosses two context
switches (EMAC ISR → `emac_rx` task → `tcpip_thread`) before stamping while
`s_tx` runs inline, so `s_rx > s_tx` is likely, giving a *positive* bias —
opposite to what's observed.

**April vs August, done correctly.** (internet S1) − (Caesium), medians:
0.97 ms in April, 0.81 ms now. Stable across 5 months, a config change, a
reboot, and different server IPs. Nothing is drifting.

---

## Phase 5 — Topology

`ip route get 192.168.71.54` → **via 192.168.72.1**. serv1 is `192.168.75.3/22`
(covers .72.0–.75.255); Caesium at `.71.54` is outside it. The path is
**routed, not switched**: serv1 → switch → UniFi gateway → switch → ESP32.

Ping evidence:

```
ICMP to Caesium    min 0.407  avg 0.488  max 0.742  mdev 0.041 ms
ICMP to gateway    min 0.127  avg 0.227  max 4.394  mdev 0.320 ms
NTP delta          min 0.504  (chrony, kernel stamps)
```

Three readings: the **floor is the ESP32** (0.407 vs 0.127 to a router one hop
closer); NTP is ~100 µs slower than ICMP to the same device, pricing the
post-stamp TX path; and the network gear is **jittery, not biased** — the
gateway's own control-plane replies wander (4.4 ms max) but traffic *forwarded*
through it is tight (mdev 0.041 ms). Queueing is non-negative both ways and
chrony's clock filter takes minimum-delay samples, so it cannot manufacture a
one-directional bias.

`traceroute` to NIST and Google share only hops 1–3 —
`192.168.72.1` → `207.225.112.10` → `63.225.124.73` — then diverge. A constant
absolute bias across both despite divergent onward paths points at that shared
segment.

---

## Phase 6 — Hardware timestamping feasibility

**serv1 can do it today.** `ethtool -T eno1` reports `hardware-transmit`,
`hardware-receive`, a PTP hardware clock, and RX filter mode `all` — that last
one is what chrony needs to stamp non-PTP packets. `hwtimestamp eno1` works
now, unused.

**Caesium cannot.** Searched and confirmed: the original ESP32 EMAC has no IEEE
1588 unit. Espressif's kostaond in esp-idf #13423: *"ESP32 does not support
hardware time stamping in MAC module"* and *"IEEE1588 is not supported by ESP32.
The ESP32 TRM used to list it as supported feature in past but it was a
mistake."* Only ESP32-P4 has the silicon. This explains esp32.com t=30889, where
someone finds `TxTimestampStatus` permanently 0.

No separate NIC firmware exists either — the MAC is on-die and the LAN8720A is a
PHY with no CPU. So any driver work is ordinary C in the project's own binary.
The obstacle is that `framework = arduino` ships `esp_eth` as a precompiled
`.a`; patching means moving to `framework = espidf`.

**The protocol question.** Client-side and server-side *RX* stamping are purely
local — no wire change, works with any peer. Server-side *TX* stamping cannot
be local (you can't put a packet's departure time inside that packet), so it
needs [RFC 9769 — NTP Interleaved Modes](https://www.rfc-editor.org/rfc/rfc9769.html)
(May 2025). Friendly shape: no header change, no extension fields, implicit
negotiation via the origin timestamp, graceful fallback to basic mode. chrony
4.2 implements it as `xleave`; currently `Interleaved : No`.

**The asymmetry trap.** Because bias is a difference, a one-sided fix can be a
net loss. With `s_rx ≈ 290 µs`, `s_tx ≈ 100 µs`: today ≈ +95 µs; **TX fixed
alone ≈ +145 µs, worse**; RX alone ≈ −50 µs; both ≈ 0. The TX side is the one
you'd naturally reach for first, and it's the harmful one alone.

---

## Phase 7 — Reference clock options

serv1 has a real 16550A at `0x3F8` (`ttyS0`, idle, verified silent at 9600 and
38400), `pps_ldisc` and `pps_gpio` modules present, chrony built `+REFCLOCK`.
No GPS is attached (`/sys/class/pps/` empty, no `/dev/pps*`, no USB serial, no
`gpsd`). So PPS-into-DCD is available: `ldattach PPS /dev/ttyS0` +
`refclock PPS /dev/pps0`. chrony's PPS driver needs a companion source for the
integer second, but the existing network servers serve that fine — their
sub-second error is discarded.

**Shared PPS between the two machines — abandoned.** Would buy ~30 ns of
common-mode cancellation against an 800,000 ns question, at the cost of a 5.5 m
run, ground-loop risk against the PoE-**ISO** isolation, and level shifting.
Also established: never put a microcontroller in a PPS path — it *regenerates*
the edge with µs-scale variable latency, injecting exactly the error class being
measured. Buffer, never regenerate. Wire length is a non-issue regardless:
~5 ns/m, so 5.5 m is 27.5 ns and you'd need ~160 km to matter.

---

## Phase 8 — Where it landed

Two hypotheses remain, **observationally identical from serv1**:

- **(a)** shared-segment WAN asymmetry ≥1.12 ms upstream-slower
- **(b)** Caesium genuinely 0.78 ms slow

Both predict what we see. The tiebreaker is *mechanism*, not data: (a) has
plausible ones; (b) has none, since the device is capped at 0.25 ms and the GPS
is bounded at µs. But that is inference from bounds, and the GPS time base was
never actually measured — the weakest link.

Chosen next action: a **time-base validation pulse** on GPIO33 (verified free
against `ESP32-POE-ISO-PINOUT.png`; GPIO17 is the Ethernet clock and isn't led
out), scoped against the PPS on GPIO16. Pulse landing after the PPS edge =
Caesium running slow by that much. Full code and decision tree in HANDOFF.md.

---

## Corrections made during the session

Recorded because they explain why the conclusions are shaped as they are.

| Claim | Correction |
|---|---|
| "Bias can't be network asymmetry — it doesn't scale with RTT" | **Wrong.** Asymmetry in the *shared last mile* is a constant absolute offset regardless of remote distance. Exactly this signature. |
| "The differential roughly doubled since April" | **Wrong.** Compared Caesium's raw offset across two discipline regimes. Like-for-like, it's stable: 0.97 → 0.81 ms. |
| "Delay/jitter slightly better than April" | Marginally *worse* (0.693→0.701 ms, 0.020→0.024 ms), inside run-to-run noise. |
| "DesignWare MACs generally include a 1588 unit, so this is plausible" | **Wrong for ESP32.** No unit exists; the TRM listing it was an erratum. |
| "Five independent stratum-1 sources agree" | Google is unusable: anycast, ICMP min 2.22 ms vs NTP peer delay 13.78 ms. Effective references: **two organisations**. |
| "One switch between host and device" | **Routed, not switched** — different subnet, via the gateway. |
| "Sharing one GPS is better (common-mode cancellation)" | Over-sold. The cancelled error is ~30 ns against 800,000 ns — irrelevant at this scale. |
| User: "you don't have root, I may need to make the chrony change" | Passwordless sudo was available; the change had already been made. |

---

## Changes made to the system

- `/etc/chrony/sources.d/caesium-ntp-server.sources` — removed the `noselect`
  Caesium entry. File now empty; original saved as `.bak` alongside.
  `nist.sources` untouched. No chronyd restart needed (chrony was already
  discarding the duplicate).
- **No firmware changes.** No code in `src/` was modified.
- Files created: `report_20260830.log`, `HWTIMESTAMPING.md`, `HANDOFF.md`,
  `session.md`.

## Still open

Resolved since this was written (2026-08-31):

- ~~`TODO.md` soak item unchecked~~ — now ticked, 28.8 days behind it.
- ~~`HANDOFF.md`, `HWTIMESTAMPING.md`, `session.md` untracked~~ — all tracked
  and on `main`. `report_*.log` remain gitignored and did not travel, which is
  why the numbers were inlined into HANDOFF.md.
- ~~The open question of (a) WAN asymmetry vs (b) Caesium running slow~~ —
  **resolved as (a)**. The time-base validation pulse measured `E_c` at
  +5.07 us over 600 s, so the device is ~160x too small to explain the 0.81 ms
  gap. Method and data in [TIMEBASE.md](TIMEBASE.md).

Genuinely still open:

- `AGENTS.md` points at `~/.platformio/penv/bin/pio`, which exists on the Mac
  but not on serv1. Fine as written for the machine that builds.
- The PPS is still trusted against UTC on the NEO-M9N's ~30 ns spec. Closing
  that needs the independent reference clock parked in HANDOFF.md.
- The device's packet-path term is *bounded* at 0.25 ms, not measured.
  Measuring it is what RFC 9769 interleaved mode plus RX timestamping buys.
