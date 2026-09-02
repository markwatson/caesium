/*
 * Caesium - GPS-disciplined NTP Time Server
 *
 * This firmware implements a precise time source using:
 * - SparkFun NEO-M9N GPS module for time reference (via UART)
 * - PPS (Pulse Per Second) interrupt for microsecond accuracy
 * - ESP32-PoE-ISO (Olimex) as the microcontroller
 *
 * Architecture:
 * - PPS ISR: Captures esp_timer_get_time() on rising edge (exact second boundary)
 * - PVT Callback: Fires when GPS sends time data via UART, pairs it with PPS
 * - NTP: lwIP raw UDP callback in tcpip_thread (no socket/task overhead)
 * - Core 1: Main loop drives checkUblox() to process UART and trigger callbacks
 */

#include <ESPmDNS.h>
#include <ETH.h>

#include "SparkFun_u-blox_GNSS_Arduino_Library.h"
#include "gps_time.h"
#include "ntp.h"

#ifdef TIMEBASE_PULSE
/*
 * Time-base validation pulse (debug builds only). Fires one *served* second
 * after the PPS the NTP path is interpolating from; scoped against the real
 * PPS on GPIO16, a pulse landing late means Caesium is serving slow by that
 * much. Both pins are in the out1 bank, so one write drives them with no skew.
 * Method, wiring and results: TIMEBASE.md.
 */
#include "soc/gpio_struct.h"

#define TIMEBASE_PIN 33     // primary, 2 pins above the PPS on the EXT header
#define TIMEBASE_PIN_ALT 32 // secondary, directly adjacent to the PPS pin
#define TIMEBASE_BIT ((1UL << (TIMEBASE_PIN - 32)) | (1UL << (TIMEBASE_PIN_ALT - 32)))
#define TIMEBASE_ARM_WINDOW_US 2000 // only busy-wait when this close
#define TIMEBASE_PULSE_WIDTH_US 50

static uint32_t timebasePulseCount = 0;

// Cheap to call every loop() iteration; returns immediately in the common case.
static void emitTimebasePulse() {
  static uint32_t lastPulseSec = 0;

  TimeState s;
  getTimeStateAtomic(s);
  if (!s.valid || s.epochSec == lastPulseSec) {
    return;
  }

  // One served second after the reference PPS, using the same calibrated
  // interval that hwTimeToNtp() uses. If the crystal calibration is wrong,
  // this pulse is wrong by exactly the same amount the served time is.
  int64_t target = s.ppsTimeMicros + (int64_t)s.usPerPps;
  int64_t delta = target - esp_timer_get_time();
  if (delta <= 0 || delta > TIMEBASE_ARM_WINDOW_US) {
    return; // too early to arm, or already missed it this second
  }

  lastPulseSec = s.epochSec;
  while (esp_timer_get_time() < target) {
    // Busy-wait, sub-microsecond. Bounded by TIMEBASE_ARM_WINDOW_US.
  }
  GPIO.out1_w1ts.val = TIMEBASE_BIT;
  delayMicroseconds(TIMEBASE_PULSE_WIDTH_US);
  GPIO.out1_w1tc.val = TIMEBASE_BIT;
  timebasePulseCount++;
}
#endif // TIMEBASE_PULSE

// Hostname used for both ETH and mDNS
const char HOSTNAME[] = "caesium";

// GPS module instance
SFE_UBLOX_GNSS myGNSS;

// UART pins for GNSS module (ESP32-PoE-ISO Serial1)
// TX1=GPIO4 (ESP32 TX -> GNSS RX), RX1=GPIO36 (GNSS TX -> ESP32 RX)
#define GPS_TX_PIN 4
#define GPS_RX_PIN 36
#define GPS_BAUD 38400

// Ethernet connection state
static bool ntpServerStarted = false;

// Ethernet event handler
void onEthEvent(arduino_event_id_t event) {
  switch (event) {
  case ARDUINO_EVENT_ETH_START:
    Serial.println(F("[ETH] Started"));
    ETH.setHostname(HOSTNAME);
    break;

  case ARDUINO_EVENT_ETH_CONNECTED:
    Serial.println(F("[ETH] Link up"));
    break;

  case ARDUINO_EVENT_ETH_GOT_IP:
    Serial.printf("[ETH] Got IP: %s (MAC: %s, %dMbps %s)\n",
                  ETH.localIP().toString().c_str(), ETH.macAddress().c_str(),
                  ETH.linkSpeed(),
                  ETH.fullDuplex() ? "Full Duplex" : "Half Duplex");
    if (!ntpServerStarted) {
      initNtpServer();
      ntpServerStarted = true;
    }
    break;

  case ARDUINO_EVENT_ETH_DISCONNECTED:
    Serial.println(F("[ETH] Disconnected"));
    break;

  case ARDUINO_EVENT_ETH_STOP:
    Serial.println(F("[ETH] Stopped"));
    break;

  default:
    break;
  }
}

// Configure the GPS TimePulse (PPS) output
void configureTimePulse() {
  UBX_CFG_TP5_data_t tpData;

  if (!myGNSS.getTimePulseParameters(&tpData)) {
    Serial.println(F("[GPS] ERROR: Failed to get TimePulse parameters"));
    return;
  }

  Serial.println(F("[GPS] Configuring TimePulse (PPS)..."));

  tpData.tpIdx = 0; // TIMEPULSE (not TIMEPULSE2)

  // Before GNSS lock: no pulse
  tpData.freqPeriod = 0;
  tpData.pulseLenRatio = 0;

  // After GNSS lock: 1Hz with 100ms pulse
  tpData.freqPeriodLock = 1000000;   // 1 second period (1Hz)
  tpData.pulseLenRatioLock = 100000; // 100ms pulse length

  // Configure flags
  tpData.flags.bits.active = 1;         // Enable time pulse
  tpData.flags.bits.lockGnssFreq = 1;   // Sync to GNSS when available
  tpData.flags.bits.lockedOtherSet = 1; // Use freqPeriodLock when locked
  tpData.flags.bits.isFreq = 0;         // freqPeriod is period (not frequency)
  tpData.flags.bits.isLength = 1;    // pulseLenRatio is length (not duty cycle)
  tpData.flags.bits.alignToTow = 1;  // Align pulse to top of second
  tpData.flags.bits.polarity = 1;    // Rising edge at top of second
  tpData.flags.bits.gridUtcGnss = 0; // Align to UTC
  tpData.flags.bits.syncMode = 0;    // Sync mode

  if (myGNSS.setTimePulseParameters(&tpData)) {
    Serial.println(
        F("[GPS] PPS configured: 1Hz after lock, rising edge aligned to ToS"));
  } else {
    Serial.println(F("[GPS] ERROR: Failed to set TimePulse parameters"));
  }
}

void setup() {
  delay(500); // Wait for hardware to stabilize

  Serial.begin(115200);
  Serial.println(F("\n\n========================================"));
  Serial.println(F("       Caesium GPS Time Server"));
  Serial.println(F("========================================\n"));

  // Initialize GPS via UART (Serial1)
  Serial.println(F("[INIT] Starting GNSS UART..."));
  Serial1.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

  Serial.println(F("[INIT] Connecting to GPS module..."));
  if (!myGNSS.begin(Serial1)) {
    Serial.println(F("[INIT] ERROR: GPS module not detected! Check wiring."));
    while (1)
      delay(1000);
  }
  Serial.println(F("[INIT] GPS module connected via UART"));

  // Configure UART port for UBX protocol only (disable NMEA for efficiency)
  myGNSS.setUART1Output(COM_TYPE_UBX);

  // Set navigation rate to 1Hz (one PVT message per PPS pulse)
  myGNSS.setNavigationFrequency(1);

  // Register PVT callback — the library calls this when a complete,
  // checksum-verified UBX-NAV-PVT packet has been assembled from UART
  if (myGNSS.setAutoPVTcallbackPtr(&pvtCallback)) {
    Serial.println(F("[INIT] PVT callback registered"));
  } else {
    Serial.println(F("[INIT] WARNING: setAutoPVTcallbackPtr failed!"));
  }

  // Configure PPS output on GPS module
  configureTimePulse();

  // Initialize PPS interrupt
  initGpsTime(PPS_PIN);

#ifdef TIMEBASE_PULSE
  pinMode(TIMEBASE_PIN, OUTPUT);
  digitalWrite(TIMEBASE_PIN, LOW);
  pinMode(TIMEBASE_PIN_ALT, OUTPUT);
  digitalWrite(TIMEBASE_PIN_ALT, LOW);
  Serial.printf("[INIT] Time-base validation pulse enabled on GPIO%d and GPIO%d\n",
                TIMEBASE_PIN, TIMEBASE_PIN_ALT);
#endif

  // Initialize Ethernet (NTP server starts when we get an IP)
  Serial.println(F("[INIT] Starting Ethernet..."));
  Network.onEvent(onEthEvent);
  ETH.begin();

  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("MDNS responder started, listening on %s.local\n", HOSTNAME);
  }

  Serial.println(F("[INIT] Setup complete\n"));
}

void loop() {
  beginUartCycle(); // must precede checkUblox(); see gps_time.h

  // Drive the SparkFun library: read UART bytes and assemble packets,
  // then fire any pending callbacks (e.g. pvtCallback).
  myGNSS.checkUblox();
  myGNSS.checkCallbacks();

  if (ppsTriggered) {
    ppsTriggered = false;

    if (!isTimeValid()) {
      Serial.printf("[PPS] #%lu | Waiting for time sync...\n", ppsCount);
    } else {
      // Log crystal calibration every 60 seconds
      static uint32_t lastCalibLog = 0;
      if (ppsCount - lastCalibLog >= 60) {
        lastCalibLog = ppsCount;
        uint32_t cal = getCrystalCalibration();
        if (cal != 0) {
          int32_t driftPpm = (int32_t)cal - 1000000;
          Serial.printf("[DRIFT] Crystal: %lu us/pps (%+ld ppm)\n",
                        (unsigned long)cal, (long)driftPpm);
          uint32_t dropped = getDroppedPairingCount();
          if (dropped > 0) {
            Serial.printf("[PAIRING] %lu publishes dropped (starved across a "
                          "second boundary)\n",
                          (unsigned long)dropped);
          }
#ifdef TIMEBASE_PULSE
          Serial.printf("[TIMEBASE] %lu pulses emitted on GPIO%d+%d\n",
                        (unsigned long)timebasePulseCount, TIMEBASE_PIN,
                        TIMEBASE_PIN_ALT);
#endif
        }
      }
    }
  }

  // After a successful PPS+PVT sync, the UART is idle for ~900ms.
  // This is the safe window for additional UART polls that would
  // otherwise block checkUblox().
  if (consumeSyncEvent()) {
    static uint32_t lastLeapPoll = 0;
    if (ppsCount - lastLeapPoll >= 3600) {
      lastLeapPoll = ppsCount;
      int32_t timeToEvent;
      uint8_t li = myGNSS.getLeapIndicator(timeToEvent);
      if (li <= 2) {
        setLeapIndicator(li);
        if (li > 0) {
          Serial.printf("[LEAP] Warning: leap second %s in %ld seconds\n",
                        li == 1 ? "insert" : "delete", (long)timeToEvent);
        }
      }
    }
  }

#ifdef TIMEBASE_PULSE
  emitTimebasePulse();
#endif

#ifdef STARVE_TEST
  /*
   * Deliberately starve loop() across a second boundary to exercise the
   * PPS/PVT pairing guards. Never ship this. Stalling 1050ms every ~6s walks
   * the resume phase forward 50ms each time, sweeping the narrow window
   * instead of waiting to hit it by luck.
   */
  static uint32_t lastStarveAt = 0;
  if (ppsCount - lastStarveAt >= 6) {
    lastStarveAt = ppsCount;
    delay(1050);
  }
#endif

  // Yield briefly so other FreeRTOS tasks can run
  vTaskDelay(1);
}
