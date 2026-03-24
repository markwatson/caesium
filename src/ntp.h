#ifndef NTP_H
#define NTP_H

#include <Arduino.h>

// NTP Configuration
#define NTP_PORT 123
#define NTP_PACKET_SIZE 48

// NTP Epoch offset: seconds between 1900-01-01 and 1970-01-01
#define NTP_UNIX_OFFSET 2208988800UL

/**
 * NTP Packet Structure (48 bytes)
 * Supports both NTPv3 (RFC 1305) and NTPv4 (RFC 5905)
 */
struct __attribute__((packed)) NtpPacket {
  uint8_t li_vn_mode;
  uint8_t stratum;
  uint8_t poll;
  int8_t precision;
  uint32_t rootDelay;
  uint32_t rootDispersion;
  uint8_t refId[4];
  uint32_t refTimestamp_s;
  uint32_t refTimestamp_f;
  uint32_t origTimestamp_s;
  uint32_t origTimestamp_f;
  uint32_t rxTimestamp_s;
  uint32_t rxTimestamp_f;
  uint32_t txTimestamp_s;
  uint32_t txTimestamp_f;
};

// LI (Leap Indicator) values (RFC 5905 Figure 9)
#define NTP_LI_NONE 0    // No warning
#define NTP_LI_ADD_SEC 1  // Last minute of the day has 61 seconds
#define NTP_LI_SUB_SEC 2  // Last minute of the day has 59 seconds
#define NTP_LI_ALARM 3    // Clock unsynchronized

// Mode values
#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4

// Stratum values
#define NTP_STRATUM_UNSPECIFIED 0
#define NTP_STRATUM_PRIMARY 1

inline uint8_t makeNtpFlags(uint8_t li, uint8_t vn, uint8_t mode) {
  return (li << 6) | (vn << 3) | mode;
}

inline uint8_t getNtpVersion(uint8_t li_vn_mode) {
  return (li_vn_mode >> 3) & 0x07;
}

inline uint8_t getNtpMode(uint8_t li_vn_mode) { return li_vn_mode & 0x07; }

/**
 * Initialize the NTP server using the lwIP raw UDP API.
 * The receive callback executes directly in the tcpip_thread context,
 * eliminating context-switch jitter from the socket API.
 * Call after network stack is initialized (e.g. after ETH.begin()).
 */
void initNtpServer();

#endif // NTP_H
