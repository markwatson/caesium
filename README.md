# Caesium — GPS-Disciplined NTP Time Server

> **Disclaimer:** This is a hobby/learning project. It works well enough for home lab use, but has not been rigorously validated for production or safety-critical timekeeping. There may be bugs.

Homebrew Stratum-1 NTP server using an ESP32-PoE-ISO and SparkFun NEO-M9N GPS module.

I built this because I got bored and wanted to try to build an accurate NTP server for using on my local network. There are probably some valid use cases for this such as machines needing higher accurate time than can be provided over the network. I didn't really have a good use case other than that it seemed cool to build a stratum 1 time server.

## Features

- Stratum-1 NTP server using GPS PPS for microsecond-accurate timekeeping
- lwIP raw UDP API for low-jitter NTP response (no socket/task overhead)
- Push/callback GPS architecture: PPS ISR captures the second boundary, UBX-NAV-PVT callback identifies which second
- Crystal drift compensation: measures PPS-to-PPS intervals and corrects sub-second interpolation
- Leap second forwarding: polls UBX-NAV-TIMELS hourly and sets NTP LI bits per RFC 5905
- Staleness detection: reports unsynchronized (LI=3) if no PPS+PVT sync for 5 seconds
- Ethernet connectivity with mDNS (`caesium.local`)

## Hardware

### Bill of Materials

| Part | Notes |
|------|-------|
| [Olimex ESP32-PoE-ISO](https://www.olimex.com/Products/IoT/ESP32/ESP32-POE-ISO/open-source-hardware) | ESP32 with hardware Ethernet (LAN8720 PHY). May work on other ESP32 boards but wired Ethernet is important for low-jitter NTP. |
| [SparkFun NEO-M9N GPS Breakout (SMA)](https://www.sparkfun.com/sparkfun-gps-breakout-neo-m9n-sma-qwiic.html) | u-blox NEO-M9N with PPS output. Other SparkFun u-blox breakouts likely work with minor code changes. |
| [GPS Antenna (SMA)](https://www.adafruit.com/product/960) | Any active or passive GPS antenna with SMA connector. |
| Steel plate / ground plane | Improves GPS antenna reception. A metal shelf or mounting plate works. |

### Wiring

| GPS Breakout | ESP32-PoE-ISO | Notes |
|--------------|---------------|-------|
| 3V3 | 3V3 | Power |
| GND | GND | Ground |
| PPS | GPIO 16 | 1Hz pulse, rising edge = top of second |
| TX | GPIO 36 | GPS transmits → ESP32 receives (UART1 RX) |
| RX | GPIO 4 | ESP32 transmits → GPS receives (UART1 TX) |

## Building

Requires [PlatformIO](https://platformio.org/) 🇺🇦.

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

## Long-term Testing

See [TESTING.md](TESTING.md) for how to evaluate Caesium against public NTP servers using chrony on a Linux client. Includes chrony configuration, multi-day data collection, and an analysis script (`analyze_chrony.py`) that parses chrony logs and generates a comparison report.

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
