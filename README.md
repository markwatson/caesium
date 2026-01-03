# Caesium — GPS-Disciplined NTP Time Server

Homebrew Stratum-1 NTP server using an ESP32-PoE-ISO and SparkFun NEO-M9N GPS module.

## Features

- Stratum-1 NTP server using GPS + PPS for accurate time
- UDP NTP server (port 123) implemented using lwIP sockets
- Ethernet connectivity with mDNS advertising as `caesium.local`
- Lightweight dual-core design: GPS polling and PPS handling on Core 1, NTP server on Core 0

## Hardware

- **ESP32-PoE-ISO** (Olimex) or equivalent ESP32 board with Ethernet
- **SparkFun NEO-M9N** GPS module (I2C) with PPS output wired to GPIO36

## Building

Requires [PlatformIO](https://platformio.org/) 🇺🇦.

```bash
~/.platformio/penv/bin/pio run
```

## Usage

The device advertises itself via mDNS as `caesium.local`. Test with the included Python scripts:

```bash
# Single NTP query test
python3 test_ntp.py caesium.local

# Continuous monitoring with statistics
python3 test_stability.py caesium.local
```

You can also use standard tools like `sntp`:

```bash
sntp caesium.local
```

## Notes

- The server reports Stratum 1 when GPS is locked; unsynchronized (`LI=3`) otherwise
- Without hardware packet timestamping, expect tens of milliseconds of offset/jitter
- Local NTP clients (chrony/ntpd) will converge and discipline the system clock
- `test_ntp.py` prints round-trip delay and clock offset estimates
- `test_stability.py` monitors continuously and collects statistics over time

## License

MIT (see [LICENSE](LICENSE))

## Dependencies

- [OLIMEX ESP32-POE-ISO](https://github.com/OLIMEX/ESP32-POE-ISO)
- [SparkFun u-blox GNSS Arduino Library](https://github.com/sparkfun/SparkFun_u-blox_GNSS_Arduino_Library)
