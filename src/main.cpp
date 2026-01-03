/*
 * Caesium - GPS-disciplined NTP Time Server
 *
 * This firmware implements a precise time source using:
 * - SparkFun NEO-M9N GPS module for time reference
 * - PPS (Pulse Per Second) interrupt for microsecond accuracy
 * - ESP32-PoE-ISO (Olimex) as the microcontroller
 *
 * Architecture:
 * - Core 0: Reserved for networking (ETH/WiFi stack)
 * - Core 1: Application code (main loop + GPS polling task)
 * - PPS ISR: Hardware interrupt captures exact second boundary
 *
 * The GPS polling runs as a low-priority background task so it won't
 * block the main loop. This ensures NTP responses can be handled quickly
 * even while GPS I2C communication is in progress.
 */

#include <Wire.h>

#include "SparkFun_u-blox_GNSS_Arduino_Library.h"
#include "gps_time.h"

// GPS module instance
SFE_UBLOX_GNSS myGNSS;

// I2C pins for GNSS module
#define GPS_SDA_PIN 33
#define GPS_SCL_PIN 32

// GPS polling configuration
const unsigned long GPS_POLL_INTERVAL_MS = 60000; // Poll GPS every 60 seconds
const unsigned long GPS_STARTUP_POLL_MS = 2000;   // Poll faster at startup until lock

// Status print interval
const unsigned long STATUS_PRINT_INTERVAL = 5000;
unsigned long lastStatusPrint = 0;

// Mutex for I2C access (GPS library isn't thread-safe)
SemaphoreHandle_t i2cMutex = NULL;

// Task handle for GPS polling
TaskHandle_t gpsTaskHandle = NULL;

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

// Poll GPS for time sync (called from GPS task)
void pollGpsTime() {
  // Take I2C mutex before accessing GPS
  if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    Serial.println(F("[GPS] ERROR: Could not acquire I2C mutex"));
    return;
  }

  uint32_t us;
  uint32_t epoch = myGNSS.getUnixEpoch(us);
  bool timeValid = myGNSS.getTimeValid();
  bool timeConfirmed = myGNSS.getConfirmedTime();
  byte siv = myGNSS.getSIV();

  // Release mutex as soon as I2C operations are done
  xSemaphoreGive(i2cMutex);

  // Debug output (outside mutex - Serial is thread-safe on ESP32)
  Serial.printf("[GPS] Epoch: %lu.%06lu | Valid: %d | Confirmed: %d | SIV: %d\n",
                epoch, us, timeValid, timeConfirmed, siv);

  // Sync time only if BOTH valid AND confirmed
  if (timeValid && timeConfirmed) {
    queueTimeSync(epoch);
  } else if (!isTimeValid()) {
    Serial.println(F("[GPS] Waiting for valid AND confirmed time..."));
  }
}

// GPS polling task - runs in background on Core 1
void gpsPollingTask(void *parameter) {
  Serial.println(F("[GPS_TASK] Started on Core 1"));

  // Poll faster at startup to get initial lock quickly
  unsigned long pollInterval = GPS_STARTUP_POLL_MS;

  for (;;) {
    pollGpsTime();

    // Once we have valid time, switch to slower polling interval
    if (isTimeValid()) {
      pollInterval = GPS_POLL_INTERVAL_MS;
    } else {
      pollInterval = GPS_STARTUP_POLL_MS; // Keep polling fast until lock
    }

    // Wait for next poll (use vTaskDelay for FreeRTOS-friendly delay)
    vTaskDelay(pdMS_TO_TICKS(pollInterval));
  }
}

// Print periodic system status
void printStatus() {
  Serial.println(F("----------------------------------------"));

  if (isTimeValid()) {
    uint64_t timestampMs = getAccurateTimestampMs();
    uint32_t seconds = timestampMs / 1000;
    uint32_t ms = timestampMs % 1000;

    // Get SIV with mutex protection
    byte siv = 0;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      siv = myGNSS.getSIV();
      xSemaphoreGive(i2cMutex);
    }

    Serial.printf("[STATUS] Time: %lu.%03lu | PPS: %lu | SIV: %d\n",
                  seconds, ms, ppsCount, siv);
  } else {
    Serial.printf("[STATUS] Waiting for GPS lock... | PPS: %lu\n", ppsCount);
  }
}

void setup() {
  delay(500); // Wait for hardware to stabilize

  Serial.begin(115200);
  Serial.println(F("\n\n========================================"));
  Serial.println(F("       Caesium GPS Time Server"));
  Serial.println(F("========================================\n"));

  // Create I2C mutex before any I2C operations
  i2cMutex = xSemaphoreCreateMutex();
  if (i2cMutex == NULL) {
    Serial.println(F("[INIT] ERROR: Failed to create I2C mutex!"));
    while (1) delay(1000);
  }

  // Initialize GPS via I2C
  Serial.println(F("[INIT] Starting I2C..."));
  Wire.begin(GPS_SDA_PIN, GPS_SCL_PIN);

  Serial.println(F("[INIT] Connecting to GPS module..."));
  if (!myGNSS.begin()) {
    Serial.println(F("[INIT] ERROR: GPS module not detected! Check wiring."));
    while (1) delay(1000);
  }
  Serial.println(F("[INIT] GPS module connected"));

  // Configure PPS output on GPS module
  configureTimePulse();

  // Initialize PPS interrupt
  initPPS(PPS_PIN);

  // Create GPS polling task on Core 1 with low priority
  // Priority 1 is lower than default loop() priority (1), so it won't block
  // Stack size 4KB should be plenty for GPS polling
  xTaskCreatePinnedToCore(
    gpsPollingTask,   // Task function
    "GPS_Poll",       // Task name
    4096,             // Stack size (bytes)
    NULL,             // Parameters
    1,                // Priority (low - won't block main loop)
    &gpsTaskHandle,   // Task handle
    1                 // Core 1 (same as loop, but lower priority)
  );

  Serial.println(F("[INIT] Setup complete - GPS polling task started\n"));
}

void loop() {
  // Main loop stays lightweight for fast NTP response
  // GPS polling is handled by background task

  // Check for incoming GPS data (quick, non-blocking)
  if (xSemaphoreTake(i2cMutex, 0) == pdTRUE) {
    myGNSS.checkUblox();
    xSemaphoreGive(i2cMutex);
  }

  // Handle PPS interrupt - just log for now
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

  // Print periodic status
  if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
    lastStatusPrint = millis();
    printStatus();
  }

  // TODO: NTP server handling will go here
  // The main loop is now free to respond to UDP packets quickly
  // since GPS polling happens in a background task

  // Minimal delay - just yield to other tasks briefly
  vTaskDelay(1);
}
