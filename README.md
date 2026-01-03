# Caesium — GPS-disciplined NTP Time Server

Small, homebrew NTP server using an ESP32-PoE-ISO and a GPS module (SparkFun NEO-M9N).

Features

- Stratum-1 NTP server using GPS + PPS for accurate time
- UDP NTP server (port 123) implemented using lwIP sockets
- Ethernet + mDNS advertising as `caesium.local`
- Lightweight design: GPS polling and PPS handling on Core 1, NTP server on Core 0

Hardware

- ESP32-PoE-ISO (Olimex) or equivalent ESP32 board with Ethernet
- SparkFun NEO-M9N GPS module (I2C) with PPS output wired to GPIO36

Building

Requires PlatformIO.

```bash
# Build
~/.platformio/penv/bin/pio run

# Upload and monitor (adjust environment if needed)
~/.platformio/penv/bin/pio run --target upload --environment esp32-poe-iso
~/.platformio/penv/bin/pio device monitor --environment esp32-poe-iso
```

Usage

- The device advertises itself via mDNS as `caesium.local` (hostname `caesium`).
- Test with the included Python script `test_ntp.py` (Python 3, stdlib only):

```bash
python3 test_ntp.py caesium.local
# or use IP address
python3 test_ntp.py 192.168.1.123
```

Notes and tips

- The server runs as Stratum 1 when GPS is locked; if GPS isn't locked yet it will respond with `LI=3` (unsynchronized).
- Without hardware packet timestamping, expect tens of milliseconds of offset/jitter; a local NTP client (chrony/ntpd) will converge and discipline the clock.
- `test_ntp.py` prints round-trip delay and estimated clock offset between the client and server.

License

- MIT (see LICENSE file if present)
