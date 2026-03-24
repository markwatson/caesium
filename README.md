# Caesium — GPS-Disciplined NTP Time Server

Homebrew Stratum-1 NTP server using an ESP32-PoE-ISO and SparkFun NEO-M9N GPS module.

## Features

- Stratum-1 NTP server using GPS PPS for microsecond-accurate timekeeping
- lwIP raw UDP API for low-jitter NTP response (no socket/task overhead)
- Push/callback GPS architecture: PPS ISR captures the second boundary, UBX-NAV-PVT callback identifies which second
- Crystal drift compensation: measures PPS-to-PPS intervals and corrects sub-second interpolation
- Leap second forwarding: polls UBX-NAV-TIMELS hourly and sets NTP LI bits per RFC 5905
- Staleness detection: reports unsynchronized (LI=3) if no PPS+PVT sync for 5 seconds
- Ethernet connectivity with mDNS (`caesium.local`)

## Hardware

- **ESP32-PoE-ISO** (Olimex) — dual-core ESP32 with built-in Ethernet (LAN8720 PHY)
- **SparkFun NEO-M9N** GPS module connected via UART

### Pin Assignments

| Function | GPIO | Notes |
|----------|------|-------|
| GPS TX (ESP32 → GPS RX) | 4 | UART1 TX |
| GPS RX (GPS TX → ESP32) | 36 | UART1 RX (input-only pin) |
| PPS | 16 | Rising edge = top of second |

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run
```

## Usage

The device advertises itself via mDNS as `caesium.local`. Test with the included Python scripts:

```bash
# Single NTP query
python3 test_ntp.py caesium.local

# Stability test (60 samples, 1/sec)
python3 test_ntp_stability.py caesium.local --count 60 --interval 1
```

Or with standard tools:

```bash
sntp caesium.local
```

## Architecture

```
PPS rising edge (GPIO16)
  → ISR captures esp_timer_get_time(), sets flag
  → Also measures interval from previous PPS for crystal drift calibration

~34ms later, GPS sends UBX-NAV-PVT over UART
  → checkUblox() assembles packet
  → checkCallbacks() fires pvtCallback()
  → Callback pairs PPS timestamp with GPS epoch under spinlock
  → Smooths PPS interval via EMA for drift-compensated sub-second timing

NTP request arrives over Ethernet
  → lwIP raw UDP callback fires in tcpip_thread
  → Timestamps immediately via esp_timer_get_time()
  → Reads timeState under spinlock
  → Computes NTP response using calibrated crystal frequency
  → Sends via udp_sendto()

Hourly (in post-sync UART idle window)
  → Polls UBX-NAV-TIMELS for upcoming leap second events
  → Sets NTP leap indicator (LI=1 insert / LI=2 delete)
```

## Notes

- Reports Stratum 1 when GPS is locked; unsynchronized (LI=3) if no sync for >5 seconds
- Forwards leap second warnings from GPS to NTP clients per RFC 5905
- Crystal drift is measured against PPS and compensated in sub-second interpolation
- Without hardware MAC-layer timestamping, expect ~1-3ms offset on wired LAN
- GPS UART runs at 38400 baud (NEO-M9N default), UBX protocol only

## License

MIT (see [LICENSE](LICENSE))

## Dependencies

- [SparkFun u-blox GNSS Arduino Library](https://github.com/sparkfun/SparkFun_u-blox_GNSS_Arduino_Library)
