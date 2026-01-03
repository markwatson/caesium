#include <Arduino.h>
#include <gps_time.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <ntp.h>

char UdpMessage[UDP_MESSAGE_SIZE] = "NTP server starting...";
const char *getUdpMessage() { return UdpMessage; }

/**
 * @brief Get current time as NTP timestamp
 *
 * NTP timestamps are 64-bit fixed-point: 32 bits seconds since 1900-01-01,
 * 32 bits fractional seconds. We use ppsEpoch (Unix seconds) and ppsMicros
 * to compute sub-second precision.
 */
void getNtpTimestamp(uint32_t &seconds, uint32_t &fraction) {
  // Read volatile variables atomically (disable interrupts briefly)
  portDISABLE_INTERRUPTS();
  uint32_t epoch = ppsEpoch;
  uint32_t ppsTime = ppsMicros;
  portENABLE_INTERRUPTS();

  // Calculate elapsed microseconds since last PPS
  uint32_t now = micros();
  uint32_t elapsedUs = now - ppsTime;

  // Handle micros() overflow (wraps every ~71 minutes)
  // If elapsed seems huge (> 2 seconds), assume wrap occurred
  if (elapsedUs > 2000000) {
    elapsedUs = 0; // Fallback to PPS boundary
  }

  // If we've crossed a second boundary, adjust
  uint32_t extraSeconds = elapsedUs / 1000000;
  uint32_t remainingUs = elapsedUs % 1000000;

  // Convert Unix epoch to NTP epoch (add offset)
  seconds = epoch + extraSeconds + NTP_UNIX_OFFSET;

  // Convert microseconds to NTP fractional seconds
  // NTP fraction = (microseconds / 1,000,000) * 2^32
  // = microseconds * 4294.967296
  // Approximate: (us * 4295) - (us / 8) for better precision without float
  fraction = ((uint64_t)remainingUs * 4294967296ULL) / 1000000ULL;
}

/**
 * @brief Handle incoming NTP request and send response
 *
 * @param sock Socket file descriptor
 * @param rxPacket Received NTP packet
 * @param rxLen Length of received data
 * @param clientAddr Client address structure
 * @param addrLen Length of address structure
 * @param rxSeconds Receive timestamp seconds (captured on packet arrival)
 * @param rxFraction Receive timestamp fraction
 */
void handleNtpRequest(int sock, const NtpPacket *rxPacket, int rxLen,
                      struct sockaddr_in *clientAddr, socklen_t addrLen,
                      uint32_t rxSeconds, uint32_t rxFraction) {

  // Validate packet size
  if (rxLen < NTP_PACKET_SIZE) {
    Serial.printf("NTP: Ignoring short packet (%d bytes)\n", rxLen);
    return;
  }

  // Extract client's version and mode
  uint8_t clientVersion = getNtpVersion(rxPacket->li_vn_mode);
  uint8_t clientMode = getNtpMode(rxPacket->li_vn_mode);

  // Only respond to client requests (mode 3)
  if (clientMode != NTP_MODE_CLIENT) {
    Serial.printf("NTP: Ignoring non-client mode %d\n", clientMode);
    return;
  }

  // Use same NTP version as client (support v3 and v4)
  if (clientVersion < 3)
    clientVersion = 3;
  if (clientVersion > 4)
    clientVersion = 4;

  // Build response packet
  NtpPacket txPacket;
  memset(&txPacket, 0, sizeof(txPacket));

  // Check if we have valid GPS time
  bool timeValid = isTimeValid();

  if (timeValid) {
    // Normal response - Stratum 1 GPS server
    txPacket.li_vn_mode =
        makeNtpFlags(NTP_LI_NONE, clientVersion, NTP_MODE_SERVER);
    txPacket.stratum = NTP_STRATUM_PRIMARY;
  } else {
    // Unsynchronized - set LI=3 (alarm) and stratum=0
    txPacket.li_vn_mode =
        makeNtpFlags(NTP_LI_ALARM, clientVersion, NTP_MODE_SERVER);
    txPacket.stratum = NTP_STRATUM_UNSPECIFIED;
  }

  // Poll interval (copy from client or use default)
  txPacket.poll =
      rxPacket->poll > 0 ? rxPacket->poll : 6; // 2^6 = 64 seconds default

  // Precision: -20 means 2^-20 seconds ≈ 1 microsecond (PPS gives us this)
  txPacket.precision = -20;

  // Root delay and dispersion (minimal for stratum 1)
  txPacket.rootDelay = 0;
  txPacket.rootDispersion = htonl(0x00000100); // ~15ms in NTP fixed-point

  // Reference ID: "GPS\0" for stratum 1 GPS source
  txPacket.refId[0] = 'G';
  txPacket.refId[1] = 'P';
  txPacket.refId[2] = 'S';
  txPacket.refId[3] = '\0';

  // Reference timestamp: when we last synced to GPS (use receive time as
  // approx)
  txPacket.refTimestamp_s = htonl(rxSeconds);
  txPacket.refTimestamp_f = htonl(rxFraction);

  // Originate timestamp: copy client's transmit timestamp
  txPacket.origTimestamp_s = rxPacket->txTimestamp_s;
  txPacket.origTimestamp_f = rxPacket->txTimestamp_f;

  // Receive timestamp: when we received the request (already captured)
  txPacket.rxTimestamp_s = htonl(rxSeconds);
  txPacket.rxTimestamp_f = htonl(rxFraction);

  // Transmit timestamp: capture as late as possible for accuracy
  uint32_t txSeconds, txFraction;
  getNtpTimestamp(txSeconds, txFraction);
  txPacket.txTimestamp_s = htonl(txSeconds);
  txPacket.txTimestamp_f = htonl(txFraction);

  // Send response immediately
  int err = sendto(sock, &txPacket, sizeof(txPacket), 0,
                   (struct sockaddr *)clientAddr, addrLen);

  if (err < 0) {
    Serial.printf("NTP: sendto failed: errno %d\n", errno);
  } else {
    // Update status message
    char *clientIp = inet_ntoa(clientAddr->sin_addr);
    snprintf(UdpMessage, sizeof(UdpMessage), "NTP: Responded to %s (v%d, %s)",
             clientIp, clientVersion, timeValid ? "synced" : "UNSYNC");
  }
}

void NtpServerTask(void *pvParameters) {
  // Create UDP socket
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    Serial.printf("NTP: Unable to create socket: errno %d\n", errno);
    snprintf(UdpMessage, sizeof(UdpMessage), "NTP: Socket creation failed");
    vTaskDelete(NULL);
    return;
  }

  // Allow address reuse (helpful for development/restarts)
  int optval = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

  // Bind to NTP port 123
  struct sockaddr_in serverAddr;
  serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serverAddr.sin_family = AF_INET;
  serverAddr.sin_port = htons(NTP_PORT);

  int err = bind(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
  if (err < 0) {
    Serial.printf("NTP: Bind to port %d failed: errno %d\n", NTP_PORT, errno);
    snprintf(UdpMessage, sizeof(UdpMessage), "NTP: Bind failed (port %d)",
             NTP_PORT);
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  Serial.printf("NTP Server listening on port %d\n", NTP_PORT);
  snprintf(UdpMessage, sizeof(UdpMessage), "NTP: Listening on port %d",
           NTP_PORT);

  // Receive buffer
  uint8_t rxBuffer[NTP_PACKET_SIZE + 16]; // Slightly larger for safety

  // Main server loop
  while (true) {
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    // BLOCKING: Wait for incoming packet
    int rxLen = recvfrom(sock, rxBuffer, sizeof(rxBuffer), 0,
                         (struct sockaddr *)&clientAddr, &addrLen);

    // Capture receive timestamp IMMEDIATELY for best accuracy
    uint32_t rxSeconds, rxFraction;
    getNtpTimestamp(rxSeconds, rxFraction);

    if (rxLen < 0) {
      Serial.printf("NTP: recvfrom failed: errno %d\n", errno);
      continue; // Keep trying
    }

    // Handle the NTP request
    handleNtpRequest(sock, (NtpPacket *)rxBuffer, rxLen, &clientAddr, addrLen,
                     rxSeconds, rxFraction);
  }

  // Cleanup (never reached in normal operation)
  close(sock);
  vTaskDelete(NULL);
}
