#ifndef NTP_H
#define NTP_H

#include <Arduino.h>

// NTP Configuration
#define NTP_PORT 123
#define NTP_PACKET_SIZE 48

// NTP Epoch offset: seconds between 1900-01-01 and 1970-01-01
#define NTP_UNIX_OFFSET 2208988800UL

// Status/debug message buffer
#define UDP_MESSAGE_SIZE 1024
extern char UdpMessage[UDP_MESSAGE_SIZE];
const char *getUdpMessage();

/**
 * NTP Packet Structure (48 bytes)
 * Supports both NTPv3 (RFC 1305) and NTPv4 (RFC 5905)
 */
struct __attribute__((packed)) NtpPacket {
  // Byte 0: LI (2 bits) | VN (3 bits) | Mode (3 bits)
  uint8_t li_vn_mode;

  // Byte 1: Stratum level (0=unspecified, 1=primary, 2-15=secondary)
  uint8_t stratum;

  // Byte 2: Poll interval (log2 seconds)
  uint8_t poll;

  // Byte 3: Precision (log2 seconds, signed)
  int8_t precision;

  // Bytes 4-7: Root delay (32-bit fixed point, seconds)
  uint32_t rootDelay;

  // Bytes 8-11: Root dispersion (32-bit fixed point, seconds)
  uint32_t rootDispersion;

  // Bytes 12-15: Reference ID (4 ASCII chars for stratum 1, IP for stratum 2+)
  uint8_t refId[4];

  // Bytes 16-23: Reference timestamp (64-bit NTP timestamp)
  uint32_t refTimestamp_s;
  uint32_t refTimestamp_f;

  // Bytes 24-31: Originate timestamp (client's transmit time, copied back)
  uint32_t origTimestamp_s;
  uint32_t origTimestamp_f;

  // Bytes 32-39: Receive timestamp (server's receive time)
  uint32_t rxTimestamp_s;
  uint32_t rxTimestamp_f;

  // Bytes 40-47: Transmit timestamp (server's transmit time)
  uint32_t txTimestamp_s;
  uint32_t txTimestamp_f;
};

// LI (Leap Indicator) values
#define NTP_LI_NONE 0    // No warning
#define NTP_LI_ADD_SEC 1 // Last minute has 61 seconds
#define NTP_LI_SUB_SEC 2 // Last minute has 59 seconds
#define NTP_LI_ALARM 3   // Clock not synchronized

// Mode values
#define NTP_MODE_RESERVED 0
#define NTP_MODE_SYMMETRIC_ACTIVE 1
#define NTP_MODE_SYMMETRIC_PASSIVE 2
#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4
#define NTP_MODE_BROADCAST 5

// Stratum values
#define NTP_STRATUM_UNSPECIFIED 0
#define NTP_STRATUM_PRIMARY 1 // GPS, atomic clock, etc.

/**
 * @brief Get current time as NTP timestamp
 * Converts Unix epoch + microseconds to NTP 64-bit fixed-point format
 * @param seconds Output: 32-bit seconds since 1900-01-01
 * @param fraction Output: 32-bit fractional seconds
 */
void getNtpTimestamp(uint32_t &seconds, uint32_t &fraction);

/**
 * @brief Build LI/VN/Mode byte
 * @param li Leap indicator (0-3)
 * @param vn Version number (3 or 4)
 * @param mode Mode (0-7)
 * @return Combined byte
 */
inline uint8_t makeNtpFlags(uint8_t li, uint8_t vn, uint8_t mode) {
  return (li << 6) | (vn << 3) | mode;
}

/**
 * @brief Extract version number from LI/VN/Mode byte
 */
inline uint8_t getNtpVersion(uint8_t li_vn_mode) {
  return (li_vn_mode >> 3) & 0x07;
}

/**
 * @brief Extract mode from LI/VN/Mode byte
 */
inline uint8_t getNtpMode(uint8_t li_vn_mode) { return li_vn_mode & 0x07; }

/**
 * @brief NTP Server Task
 * Listens on UDP port 123 and responds to NTP requests.
 * Runs as a FreeRTOS task.
 */
void NtpServerTask(void *pvParameters);

#endif // NTP_H
