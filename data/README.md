# Captured data — 2026-08-31

Raw logic-analyzer captures from the time-base validation measurement. Kept in
the repo because the rig was awkward to assemble and these are not cheap to
reproduce. Analysis and conclusions live in [../TIMEBASE.md](../TIMEBASE.md).

Instrument: **Saleae Logic 8**, serial `9096B49FF5A5B2E4`, 100 MS/s digital
(25 MS/s for 8-channel scans). Device: Caesium (ESP32-PoE-ISO + NEO-M9N)
running the `esp32-poe-iso-timebase` build.

## Channel map

| Channel | Signal | Shape |
|---|---|---|
| 0 | Time-base validation pulse (GPIO33 + GPIO32) | 50 us, 1 Hz |
| 1 | GPS PPS (GPIO16) | 100 ms, 1 Hz |

## CSV format

Transition lists, not sample dumps — one row per edge, `Time [s]` relative to
capture start. Times are in the **analyzer's** timebase, which runs about
-9.6 ppm against the GPS-disciplined PPS. That error is irrelevant to `E_c`
(a one-second interval) but matters for anything long. Absolute wall-clock
timestamps are deliberately absent; see the anchoring file below for why.

## Files

| File | What |
|---|---|
| `timebase_600s_edges.csv` | **The result.** 600 s, both channels. `E_c` = +5.070 us median |
| `timebase_600s_summary.json` | Computed statistics for the above |
| `timebase_60s_edges.csv` | 60 s pilot run. `E_c` = +5.080 us — agrees to 10 ns |
| `timebase_60s_summary.json` | Computed statistics for the above |
| `timebase_180s_under_load_edges.csv` | 180 s while serving 4711 NTP req/s |
| `timebase_180s_under_load_summary.json` | Load-test statistics — **read the caveat in TIMEBASE.md** |
| `timebase_180s_quiet_postfix_edges.csv` | 180 s quiet on merged firmware (incl. `b7fa5eb`). `E_c` = +5.120 us |
| `timebase_180s_quiet_postfix_summary.json` | Statistics for the above |
| `timebase_180s_quiet_postfix.sal` | Raw Logic 2 capture — reopen directly in the GUI |
| `timebase_180s_load_postfix_edges.csv` | 180 s at 4258 req/s on merged firmware |
| `timebase_180s_load_postfix_summary.json` | **0 whole-second errors in 757,859 queries**; NTP offset sd 0.084 ms |
| `pps_soak_274s_clean_edges.csv` | PPS characterisation, clean window. 0 dropped, 73.1 ns jitter |
| `pps_soak_300s_edges.csv.gz` | Full 300 s soak including the interference event at t=275.86 s |
| `pps_60s_interference_a.csv` | PPS during the interference, ~4000 spurious edges/60 s |
| `pps_60s_interference_b.csv` | Second interference capture |
| `pps_60s_interference_summary.json` | Shows how width-matching recovers the PPS |
| `starvation_prefix_periodic.json` | Pre-fix under induced starvation: **1005 whole-second errors in 5990 samples** |
| `starvation_postfix_periodic.json` | Same injection, with the gap guard: **0 errors**, offset sd 0.242 ms |
| `starvation_prefix.json` | Continuous-starvation variant, pre-fix: 803 errors |
| `starvation_postfix.json` | Continuous-starvation variant, fixed: 0 errors, device correctly reports LI=3 |
| `starvation_production_normal.json` | Production build, no injection: 4294 samples, 0 errors, sd 0.191 ms |
| `starvation_release_final.json` | Final shipped build: 7139 samples, 0 errors, 0 unsynced, 0 failed |
| `starvation_network_recheck.json` | Re-check over the normal network path: 3302 samples, 0 errors, 0 unsynced |
| `anchor_test_phases.json` | 8 captures of the same PPS in host-clock phase. **115.7 ms spread** — why absolute timestamps cannot anchor this measurement |

## Reproducing

```bash
python3 pps_capture.py --mode timebase --duration 600 --outdir data/ --json out.json
```

Requires Logic 2 running with Settings -> Automation -> MCP Server enabled, and
firmware built from the `esp32-poe-iso-timebase` env.
