/*
 * Caesium - GPS-disciplined NTP Time Server
 *
 * This firmware implements a precise time source using:
 * - SparkFun NEO-M9N GPS module for time reference
 * - PPS (Pulse Per Second) interrupt for microsecond accuracy
 * - ESP32-PoE-ISO (Olimex) as the microcontroller
 *
 * The GPS module provides time via I2C, while the PPS signal triggers
 * an interrupt at the exact start of each second, allowing sub-millisecond
 * time accuracy.
 */

#include <Wire.h>

#include "SparkFun_u-blox_GNSS_Arduino_Library.h"
#include "gps_time.h"

// GPS module instance
SFE_UBLOX_GNSS myGNSS;

// I2C pins for GNSS module
#define GPS_SDA_PIN 33
#define GPS_SCL_PIN 32

// Timing for periodic tasks
unsigned long lastGpsPoll = 0;
unsigned long lastStatusPrint = 0;

const unsigned long GPS_POLL_INTERVAL = 60500;    // Poll GPS every 60.5 seconds
const unsigned long STATUS_PRINT_INTERVAL = 5000; // Print status every 5 seconds

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
  tpData.flags.bits.isLength = 1;       // pulseLenRatio is length (not duty cycle)
  tpData.flags.bits.alignToTow = 1;     // Align pulse to top of second
  tpData.flags.bits.polarity = 1;       // Rising edge at top of second
  tpData.flags.bits.gridUtcGnss = 0;    // Align to UTC
  tpData.flags.bits.syncMode = 0;       // Sync mode

  if (myGNSS.setTimePulseParameters(&tpData)) {
    Serial.println(F("[GPS] PPS configured: 1Hz after lock, rising edge aligned to ToS"));
  } else {
    Serial.println(F("[GPS] ERROR: Failed to set TimePulse parameters"));
  }
}

// Print periodic system status
void printStatus() {
  Serial.println(F("----------------------------------------"));

  if (isTimeValid()) {
    uint64_t timestampMs = getAccurateTimestampMs();
    uint32_t seconds = timestampMs / 1000;
    uint32_t ms = timestampMs % 1000;

    Serial.printf("[STATUS] Time: %lu.%03lu | PPS: %lu | SIV: %d\n",
                  seconds, ms, ppsCount, myGNSS.getSIV());
  } else {
    Serial.printf("[STATUS] Waiting for GPS lock... | PPS: %lu | SIV: %d\n",
                  ppsCount, myGNSS.getSIV());
  }
}

// Poll GPS for time sync (called every GPS_POLL_INTERVAL)
void pollGpsTime() {
  uint32_t us;
  uint32_t epoch = myGNSS.getUnixEpoch(us);

  bool timeValid = myGNSS.getTimeValid();
  bool timeConfirmed = myGNSS.getConfirmedTime();
  bool timeFullyResolved = myGNSS.getTimeFullyResolved();

  // Debug: show GPS-reported time status
  Serial.printf("[GPS] Epoch: %lu.%06lu | Valid: %d | Confirmed: %d | Resolved: %d\n",
                epoch, us, timeValid, timeConfirmed, timeFullyResolved);

  // Sync time only if BOTH valid AND confirmed
  if (timeValid && timeConfirmed) {
    queueTimeSync(epoch);
  } else if (!isTimeValid()) {
    Serial.println(F("[GPS] Waiting for valid AND confirmed time..."));
  }
}

void setup() {
  delay(500); // Wait for hardware to stabilize

  Serial.begin(115200);
  Serial.println(F("\n\n========================================"));
  Serial.println(F("       Caesium GPS Time Server"));
  Serial.println(F("========================================\n"));

  // Initialize GPS via I2C
  Serial.println(F("[INIT] Starting I2C..."));
  Wire.begin(GPS_SDA_PIN, GPS_SCL_PIN);

  Serial.println(F("[INIT] Connecting to GPS module..."));
  if (!myGNSS.begin()) {
    Serial.println(F("[INIT] ERROR: GPS module not detected! Check wiring."));
    while (1) {
      delay(1000);
    }
  }
  Serial.println(F("[INIT] GPS module connected"));

  // Configure PPS output on GPS module
  configureTimePulse();

  // Initialize PPS interrupt
  initPPS(PPS_PIN);

  Serial.println(F("[INIT] Setup complete - waiting for GPS lock...\n"));
}

void loop() {
  // Process incoming GPS data
  myGNSS.checkUblox();

  // Handle PPS interrupt
  if (ppsTriggered) {
    ppsTriggered = false;

    if (isTimeValid()) {
      uint64_t timestampMs = getAccurateTimestampMs();
      uint32_t msOffset = timestampMs % 1000;
      Serial.printf("[PPS] #%lu | Epoch: %lu (latency: %lums)\n",
                    ppsCount, ppsEpoch, msOffset);
    } else {
      Serial.printf("[PPS] #%lu | Waiting for time sync...\n", ppsCount);
    }
  }

  // Poll GPS for time updates
  if (millis() - lastGpsPoll >= GPS_POLL_INTERVAL) {
    lastGpsPoll = millis();
    pollGpsTime();
  }

  // Print periodic status
  if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
    lastStatusPrint = millis();
    printStatus();
  }

  delay(10); // Small delay to allow other tasks
}
