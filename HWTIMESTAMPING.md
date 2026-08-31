# Hardware Timestamping — Design Notes

**Status: design sketch. None of this is built.** Items marked **[UNKNOWN]** are
genuinely unresolved and need investigation before anyone plans around them.

> **2026-08-31 — this is now optional polish, not a fix.** It was written while
> the device was still a suspect for the 0.81 ms gap. It is not: `E_c` measured
> +5.07 us, and >=0.56 ms of the gap is WAN asymmetry that hardware timestamping
> on the device cannot touch. See [TIMEBASE.md](TIMEBASE.md). What this document
> would still buy is *measuring* the packet-path term rather than bounding it,
> and the asymmetry trap below (fixing TX alone makes accuracy worse) still
> applies in full.

Measurement background is in [report_20260830.log](report_20260830.log).

## Why bother

Caesium currently timestamps inside the lwIP UDP callback. That is as close to
the wire as the Arduino/lwIP stack allows, but it is not close to the wire.
Measured on 2026-08-30:

| Quantity | Value |
|---|---|
| NTP round-trip floor (chrony, kernel-stamped client) | 0.504 ms |
| ICMP round-trip floor to the same device | 0.407 ms |
| Stamped server processing (T3 − T2) | 23.7 µs |
| Estimated network share (switch + router, both ways) | ~0.10 ms |
| **Remainder inside the ESP32** | **~0.39 ms** |

That ~0.39 ms is time the packet spends inside the device *outside* the two
timestamps — invisible to NTP, and therefore uncorrectable by the client.

## The six legs

NTP can only see what happens between T2 and T3. Six legs are invisible:

| Leg | Meaning | Effect on measured offset |
|---|---|---|
| `c_tx` | client stamps T1 → packet actually on wire | server looks **ahead** |
| `d_fwd` | forward network transit | server looks **ahead** |
| `s_rx` | packet on server wire → server stamps T2 | server looks **ahead** |
| *`s_proc`* | *T2 → T3 — the only **stamped** leg* | *cancels* |
| `s_tx` | server stamps T3 → packet actually on wire | server looks **behind** |
| `d_rev` | reverse network transit | server looks **behind** |
| `c_rx` | packet on client wire → client stamps T4 | server looks **behind** |

With `E` the server's true clock error:

```
theta = E + ½[(c_tx + d_fwd + s_rx) − (s_tx + d_rev + c_rx)]
delta =       (c_tx + d_fwd + s_rx) + (s_tx + d_rev + c_rx)
```

So **bias = ½(upstream legs − downstream legs)**, and `|bias| ≤ delta/2` exactly.
`s_proc` drops out entirely — separate T2/T3 stamps are what buy that, so the
23.7 µs of packet handling already costs nothing.

**Only the difference matters, never the magnitude.** Two legs of 10 ms each
produce zero bias. This is the single most important property of the system and
it drives everything below.

## The asymmetry trap — read this before writing any code

Because bias is a *difference*, fixing one leg while leaving the other is not a
partial win. It can be a net loss.

Best current estimate of the split — note that only the **sum** (~390 µs) is
measured. `s_tx ≥ ~100 µs` is a lower bound derived from the NTP-minus-ICMP gap
(0.504 − 0.407 ms), since lwIP's `icmp_input` builds its echo reply in the
received pbuf while our TX path does `pbuf_alloc` + `memcpy` + `udp_sendto`.
The individual values are **[UNKNOWN]**:

| Scenario | `s_rx` | `s_tx` | bias = ½(`s_rx` − `s_tx`) |
|---|---|---|---|
| Today | ~290 µs | ~100 µs | **+95 µs** |
| **TX fixed only** | ~290 µs | ~0 | **+145 µs — WORSE THAN TODAY** |
| RX fixed only | ~0 | ~100 µs | **−50 µs** |
| Both fixed | ~0 | ~0 | **~0** |

The legs currently point in opposite directions and partially cancel. Some of
today's accuracy is *luck*. Removing the smaller leg destroys the cancellation
and leaves the larger one fully exposed.

**Hard rules:**

1. Never ship a one-sided fix without measuring the resulting bias against a
   local reference.
2. If only one leg can be fixed, fix the **larger** one. Present evidence says
   that is `s_rx`.
3. A calibration constant applied to the stamped value is the escape hatch if
   a leg cannot be shrunk — see "Calibration" below.

The perverse consequence: the TX side is the one people reach for first, because
interleaved mode is the interesting standards work. It is also the side that
makes things worse on its own.

## What is local, and what is on the wire

The protocol question, answered in three parts:

**1. Client-side timestamping — purely local.** `SO_TIMESTAMPING` on the socket,
NIC or kernel stamps, value returned via control message. Nothing on the wire
changes. Any NTP server works unmodified. This is what `hwtimestamp eno1` in
`chrony.conf` turns on, and this machine's e1000e already supports it
(`hardware-transmit`, `hardware-receive`, RX filter mode `all`).

**2. Server-side RX timestamping — also purely local.** The server stamps its
own receive as close to the wire as it can and puts that value in the T2 field.
Wire format unchanged, no negotiation, works with every existing client. **This
is free standards-wise and it is the larger leg.**

**3. Server-side TX timestamping — needs a protocol mode.** You cannot put a
packet's own departure time inside that packet. This is not an implementation
limitation; it is causality. Hence interleaved mode.

So: two thirds local configuration, one third a standardized mode. Not a new
protocol, and not purely local either.

## RFC 9769 — NTP Interleaved Modes

Published May 2025, updates RFC 5905. Chrony implements it as `xleave`, and the
chrony on this network (4.2) supports it.

The shape is friendlier than expected:

- **No packet header change, no extension fields.** The 48-byte header is
  untouched. The mode is expressed by assigning different *values* to the
  origin and transmit timestamp fields.
- **Implicit negotiation.** No capability exchange, no version bump.
- **Graceful fallback.** A client asking for interleaved gets a basic-mode
  response from a server that does not implement it, or from one that has lost
  its state. Chrony's docs are explicit that interleaved is compatible with
  basic-only servers.

Server algorithm, per the RFC:

- Keep **pairs of local receive and transmit timestamps** per client. The RFC
  recommends a fixed-length queue, dropping old entries, keyed by IP address
  but *not* by port.
- Detect an interleaved request when both: the request's receive timestamp is
  not equal to its transmit timestamp, **and** the request's origin timestamp
  matches the local receive timestamp of a previous request from that client.
- Interleaved response fields: origin = receive timestamp of this request;
  receive = receive timestamp of this request; transmit = actual transmission
  time of the *previous* response whose receive timestamp equals this request's
  origin timestamp.
- A server **MUST NOT** send a packet whose transmit timestamp equals its
  receive timestamp — that equality is what makes detection work.
- Transmit and receive timestamps in responses must be unique.
- The server **SHOULD** save the new timestamp pair even when it could not
  answer in interleaved mode.

Clients cannot open in interleaved mode; they must see one basic response
first, then echo the server's receive timestamp in their origin field.

## Hardware reality: this board cannot do IEEE 1588

The original ESP32 has **no 1588 timestamp unit in the EMAC**. Espressif's
kostaond, in esp-idf issue #13423: *"ESP32 does not support hardware time
stamping in MAC module"* and *"IEEE1588 is not supported by ESP32. The ESP32 TRM
used to list it as supported feature in past but it was a mistake."* Only the
ESP32-P4 has the silicon. This is why esp32.com thread t=30889 finds
`TxTimestampStatus` permanently reading 0.

There is also no separate NIC to patch. The MAC is on the ESP32 die; the
LAN8720A is a PHY with no CPU and no firmware. Everything is ordinary C in our
own binary — no vendor dependency, no second flashing step.

**So "hardware timestamping" here means ISR-boundary timestamping**, not 1588.
The name is kept because the goal and the standards shape are identical: get the
stamp as close to the wire as the silicon allows.

Realistic floor: stamping in the EMAC RX ISR and the TX-done ISR reduces both
legs to ISR latency — single-digit µs — leaving MAC/DMA/PHY time of roughly
10–20 µs. Against ~390 µs today, that is the large majority of the available
win without any 1588 hardware.

## Proposed architecture

```
EMAC RX ISR            -- stamp here, not in the UDP callback
   |  carry timestamp alongside the frame
   v
emac_rx task -> tcpip_thread -> udp_recv callback
   |  build response; T2 = the ISR stamp
   v
udp_sendto
   |
EMAC TX-done ISR       -- stamp actual departure, store in client table
   |
   +--> served as the transmit timestamp of the NEXT response (RFC 9769)
```

Layers that need patching:

| Layer | Change | Difficulty |
|---|---|---|
| `emac_esp32` RX ISR | capture `esp_timer_get_time()`, attach to frame | **[UNKNOWN]** — how to carry it up without a custom pbuf field |
| `emac_esp32` TX path | hook TX-done, stamp, correlate to the packet just sent | **[UNKNOWN]** — is TX-done per-descriptor and identifiable? |
| `ntp.cpp` | use ISR stamp for T2; client state table; RFC 9769 detect/respond | tractable |
| build system | `framework = arduino` ships `esp_eth` as a **precompiled `.a`** | move to `framework = espidf`, Arduino as component |

The build-system change is the real cost of entry, not the timestamping itself.

**[UNKNOWN]** Whether the RX timestamp can be carried up to the UDP callback
without forking lwIP's pbuf. Options not yet evaluated: a side-channel ring
buffer keyed by frame, a custom pbuf, or `LWIP_PBUF_CUSTOM_DATA` if the build
exposes it.

## Client state table

The ESP32 must keep per-client `(receive, transmit)` timestamp pairs. Sizing:

- 16 bytes per pair plus a key. A 32-entry table is ~1 KB — trivial for this
  device, and 32 concurrent NTP clients is far beyond a home LAN.
- Keyed by IP, not port, per the RFC.
- Fixed-length queue, evict oldest.
- **NAT caveat** (from chrony's docs): multiple clients behind one address share
  a table entry and clobber each other, which silently degrades them to basic
  mode. Acceptable on a LAN; worth a debug counter.

Losing state is safe — it degrades to basic mode, which is exactly today's
behaviour.

## Reference points — a subtle trap **[UNKNOWN]**

Hardware 1588 units typically stamp at the start-of-frame delimiter. An ISR
fires on DMA completion, i.e. at **end of frame**. Those differ by the frame
serialization time — about 7.2 µs for a 114-byte frame at 100 Mbps.

If our stamps are end-of-frame and the client's NIC stamps at SFD, each leg
picks up a fixed offset, and the two do not obviously cancel. This is small
relative to the ~95 µs being fixed, but it becomes the new floor, and it is
precisely the class of fixed asymmetry this whole exercise exists to remove.

Needs investigation: what reference point e1000e uses, what reference point the
ESP32 EMAC DMA-done interrupt corresponds to, and whether the residual is worth
a compile-time constant.

## Calibration

If a leg cannot be shrunk, correct the *value*, never the timing. Stamping
`T3' = T3 + k` shifts theta by `+k/2` at zero cost, and it makes `delta` more
honest: `delta = (T4−T1) − (T3−T2)`, so raising T3 moves hidden latency out of
the invisible legs into the visible stamped leg. Inserting a real delay to
"balance" the legs is strictly worse — it burns CPU in `tcpip_thread`, inflates
`delta`, and buys nothing the constant does not.

**Never calibrate against an internet reference.** Tuning `k` until the NIST
comparison reads zero would bake this network's ISP asymmetry — measured at
≥1.12 ms upstream-slower — into a device whose LAN clients never traverse that
path. Calibrate against a local reference or not at all.

## Sequencing

1. **`hwtimestamp eno1` on the client.** One line, no firmware. Sharpens every
   measurement taken afterwards. Note the date — it shifts measured delays by
   10–20 µs and will otherwise look like a device change in the logs.
2. **Local reference clock.** A Pi with a GPS HAT on kernel PPS reaches ~1–5 µs.
   Without this, none of the work below can be *verified*, only asserted.
3. **RX-side ISR stamping.** Larger leg, no protocol change, safe direction.
4. **TX-side + RFC 9769 interleaved.** Only after step 3, per the trap above.

Steps 3 and 4 both depend on the `framework = espidf` migration.

## Validation

Success is not "the offset got smaller." Required:

- `delta` floor drops from 0.504 ms toward the network-only ~0.10 ms. This is
  the direct evidence that the invisible legs shrank.
- Measured bias against the **local** GPS reference, before and after each step.
  A step that improves `delta` but worsens bias has hit the asymmetry trap.
- Interleaved mode confirmed active: `chronyc ntpdata <caesium>` reports
  `Interleaved : Yes`. It currently reports `No`.
- Basic-mode clients still work. `sntp` and a stock `ntpdate` must be unaffected.

## Open questions

- **[UNKNOWN]** Can the RX ISR timestamp reach the UDP callback without forking lwIP?
- **[UNKNOWN]** Is ESP32 EMAC TX-done per-descriptor and correlatable to a specific packet?
- **[UNKNOWN]** Reference-point mismatch (SFD vs end-of-frame) between e1000e and our ISRs.
- **[UNKNOWN]** Does `framework = espidf` with Arduino as a component still build this project cleanly?
- **[UNKNOWN]** Actual `s_rx` / `s_tx` split. Only the ~390 µs sum is measured. Cannot be resolved from the endpoints — needs the local reference or GPIO instrumentation against a scope.
- **[UNKNOWN]** Whether any of this is worth it: the local budget is capped at `delta/2` = 0.25 ms, while the residual being chased is 0.8 ms. If that residual is ISP path asymmetry, **all** of this work is invisible to LAN clients, who never traverse that path.

## References

- [RFC 9769 — NTP Interleaved Modes](https://www.rfc-editor.org/rfc/rfc9769.html)
- [RFC 5905 — NTPv4](rfc5905.txt) (in this repo)
- [esp-idf issue #13423 — IEEE 1588 on ESP32](https://github.com/espressif/esp-idf/issues/13423)
- [esp-idf Ethernet PTP example](https://github.com/espressif/esp-idf/blob/master/examples/ethernet/ptp/README.md)
- [esp32.com t=30889 — TxTimestampStatus always 0](https://esp32.com/viewtopic.php?t=30889)
- `man chrony.conf` — `xleave`, `hwtimestamp`, `clientloglimit`
