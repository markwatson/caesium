/**
 * NTP Server — lwIP Raw UDP API (RFC 5905)
 *
 * Uses the lwIP raw API instead of Berkeley sockets. The receive callback
 * fires directly inside the tcpip_thread the moment UDP parsing completes,
 * eliminating the mailbox + context-switch jitter of recvfrom().
 *
 * Packet lifecycle (old socket API):
 *   EMAC RX ISR → EMAC task → tcpip_thread → mailbox → scheduler → recvfrom()
 *   (variable ms of jitter before we timestamp)
 *
 * Packet lifecycle (raw API):
 *   EMAC RX ISR → EMAC task → tcpip_thread → our callback (timestamp here)
 *   (no mailbox, no context switch, no scheduler delay)
 */

#include <Arduino.h>
#include <gps_time.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <ntp.h>

static struct udp_pcb *ntpPcb = NULL;

// NTP short format (16.16 fixed point) for MAXDISP = 16 seconds (RFC 5905 §6)
#define NTP_MAXDISP_SHORT (16 << 16)

/**
 * Convert a hardware timestamp to NTP format using the given time state.
 */
static void hwTimeToNtp(int64_t hwTimeUs, const TimeState &state,
                        uint32_t &seconds, uint32_t &fraction) {
  int64_t elapsedUs = hwTimeUs - state.ppsTimeMicros;
  if (elapsedUs < 0) {
    elapsedUs = 0;
  }

  // Use calibrated crystal frequency instead of assuming 1,000,000 µs/s.
  // This corrects for 20-40 ppm crystal drift in the sub-second interpolation.
  uint32_t interval = state.usPerPps;

  uint32_t extraSeconds = (uint32_t)(elapsedUs / interval);
  uint32_t remainingUs = (uint32_t)(elapsedUs % interval);

  seconds = state.epochSec + extraSeconds + NTP_UNIX_OFFSET;

  // Map crystal ticks directly to NTP fraction space:
  // fraction = (remainingUs / interval) * 2^32
  fraction = (uint32_t)(((uint64_t)remainingUs << 32) / interval);
}

/**
 * Raw UDP receive callback — runs directly in tcpip_thread context.
 *
 * Per RFC 5905 §7.3 (fast_xmit), a server always responds to unicast
 * client requests, even when unsynchronized. The client checks LI/stratum
 * and discards unsynchronized responses on its own.
 */
static void ntpRecvCallback(void *arg, struct udp_pcb *upcb, struct pbuf *p,
                             const ip_addr_t *addr, u16_t port) {
  if (p == NULL) {
    return;
  }

  // TIMESTAMP IMMEDIATELY — we are in tcpip_thread, as close to the wire
  // as lwIP allows without intercepting at the MAC layer.
  int64_t rxHwTime = esp_timer_get_time();

  // Read time state once — used for both RX and TX timestamps
  TimeState state;
  getTimeStateAtomic(state);

  // Validate packet
  if (p->len < NTP_PACKET_SIZE) {
    pbuf_free(p);
    return;
  }

  const NtpPacket *rxPacket = (const NtpPacket *)p->payload;
  uint8_t clientVersion = getNtpVersion(rxPacket->li_vn_mode);
  uint8_t clientMode = getNtpMode(rxPacket->li_vn_mode);

  if (clientMode != NTP_MODE_CLIENT) {
    pbuf_free(p);
    return;
  }

  if (clientVersion < 3) clientVersion = 3;
  if (clientVersion > 4) clientVersion = 4;

  // Build response packet
  NtpPacket txPacket;
  memset(&txPacket, 0, sizeof(txPacket));

  // Originate: copy client's transmit timestamp (RFC 5905: x.org = r.xmt)
  txPacket.origTimestamp_s = rxPacket->txTimestamp_s;
  txPacket.origTimestamp_f = rxPacket->txTimestamp_f;

  // Copy addr before freeing pbuf (addr may point into p)
  ip_addr_t clientAddr = *addr;
  u16_t clientPort = port;
  pbuf_free(p);

  // Poll: copy from client (RFC 5905: x.poll = r.poll)
  txPacket.poll = rxPacket->poll;

  // Precision: 2^-20 ≈ 1µs (esp_timer_get_time resolution + PPS ISR latency)
  txPacket.precision = -20;

  // Reference ID: "GPS" for stratum 1 GPS source (RFC 5905 Figure 12)
  txPacket.refId[0] = 'G';
  txPacket.refId[1] = 'P';
  txPacket.refId[2] = 'S';
  txPacket.refId[3] = '\0';

  // Root delay: 0 for a primary server directly connected to GPS
  txPacket.rootDelay = 0;

  if (state.valid) {
    // SYNCHRONIZED — Stratum 1 GPS server
    // Use leap indicator from GPS (0=none, 1=insert, 2=delete)
    txPacket.li_vn_mode =
        makeNtpFlags(state.leapIndicator, clientVersion, NTP_MODE_SERVER);
    txPacket.stratum = NTP_STRATUM_PRIMARY;

    // Root dispersion: grows at PHI (15 ppm) since last PPS sync.
    // We sync every second, so worst case is ~15µs. Use a conservative
    // floor of ~30µs (NTP short format 0x00000002) to account for ISR
    // jitter and oscillator tolerance.
    uint32_t sinceSyncUs = (uint32_t)((rxHwTime - state.ppsTimeMicros) & 0x7FFFFFFF);
    // PHI = 15e-6 s/s. In NTP short format: us * 15 * 65536 / 1e12
    // Simplified: us / 1017033. Add floor of 2 (~30µs).
    uint32_t rootDisp = sinceSyncUs / 1017033 + 2;
    txPacket.rootDispersion = htonl(rootDisp);

    // Reference timestamp: time of last PPS sync (RFC 5905: s.reftime)
    uint32_t refSeconds, refFraction;
    hwTimeToNtp(state.ppsTimeMicros, state, refSeconds, refFraction);
    txPacket.refTimestamp_s = htonl(refSeconds);
    txPacket.refTimestamp_f = htonl(refFraction);

    // Receive timestamp: when we received the request
    uint32_t rxSeconds, rxFraction;
    hwTimeToNtp(rxHwTime, state, rxSeconds, rxFraction);
    txPacket.rxTimestamp_s = htonl(rxSeconds);
    txPacket.rxTimestamp_f = htonl(rxFraction);

    // Transmit timestamp: capture as late as possible
    uint32_t txSeconds, txFraction;
    hwTimeToNtp(esp_timer_get_time(), state, txSeconds, txFraction);
    txPacket.txTimestamp_s = htonl(txSeconds);
    txPacket.txTimestamp_f = htonl(txFraction);
  } else {
    // UNSYNCHRONIZED — LI=3 (NOSYNC), stratum=0 (RFC 5905 §7.3)
    // Timestamps are zero ("unknown", RFC 5905 §6). Client will see
    // LI=3/stratum=0 and discard. We still respond per RFC.
    txPacket.li_vn_mode =
        makeNtpFlags(NTP_LI_ALARM, clientVersion, NTP_MODE_SERVER);
    txPacket.stratum = NTP_STRATUM_UNSPECIFIED;
    txPacket.rootDispersion = htonl(NTP_MAXDISP_SHORT);
    // All timestamp fields remain zero from memset
  }

  // Allocate and send response
  struct pbuf *pOut = pbuf_alloc(PBUF_TRANSPORT, sizeof(NtpPacket), PBUF_RAM);
  if (pOut != NULL) {
    memcpy(pOut->payload, &txPacket, sizeof(NtpPacket));
    udp_sendto(upcb, pOut, &clientAddr, clientPort);
    pbuf_free(pOut);
  }
}

void initNtpServer() {
  // Raw lwIP API must be called with the tcpip core lock held
  // when called from outside the tcpip_thread context.
  LOCK_TCPIP_CORE();

  ntpPcb = udp_new();
  if (ntpPcb == NULL) {
    UNLOCK_TCPIP_CORE();
    Serial.println(F("[NTP] ERROR: udp_new() failed"));
    return;
  }

  err_t err = udp_bind(ntpPcb, IP_ADDR_ANY, NTP_PORT);
  if (err != ERR_OK) {
    udp_remove(ntpPcb);
    ntpPcb = NULL;
    UNLOCK_TCPIP_CORE();
    Serial.printf("[NTP] ERROR: udp_bind failed: %d\n", err);
    return;
  }

  udp_recv(ntpPcb, ntpRecvCallback, NULL);
  UNLOCK_TCPIP_CORE();

  Serial.printf("[NTP] Server listening on port %d (raw UDP API)\n", NTP_PORT);
}
