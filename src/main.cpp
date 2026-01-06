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

#include <ESPmDNS.h>
#include <ETH.h>
#include <Wire.h>

#include "SparkFun_u-blox_GNSS_Arduino_Library.h"
#include "gps_time.h"
#include "ntp.h"

// Hostname used for both ETH and mDNS
const char HOSTNAME[] = "caesium";

// GPS module instance
SFE_UBLOX_GNSS myGNSS;

// I2C pins for GNSS module
#define GPS_SDA_PIN 33
#define GPS_SCL_PIN 32

// Status print interval
const unsigned long STATUS_PRINT_INTERVAL = 5000;
unsigned long lastStatusPrint = 0;

// Mutex for I2C access (GPS library isn't thread-safe)
SemaphoreHandle_t i2cMutex = NULL;

// Task handle for NTP server
TaskHandle_t ntpTaskHandle = NULL;

// Ethernet connection state
static bool ethConnected = false;
static bool ntpServerStarted = false;

// Ethernet event handler
void onEthEvent(WiFiEvent_t event) {
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
    ethConnected = true;

    // Start NTP server now that network is ready
    if (!ntpServerStarted) {
      xTaskCreatePinnedToCore(NtpServerTask,  // Task function
                              "NTP_Server",   // Task name
                              4096,           // Stack size (bytes)
                              NULL,           // Parameters
                              2,              // Priority
                              &ntpTaskHandle, // Task handle
                              0               // Core 0 (networking core)
      );
      ntpServerStarted = true;
      Serial.println(F("[NTP] Server task started"));
    }
    break;

  case ARDUINO_EVENT_ETH_DISCONNECTED:
    Serial.println(F("[ETH] Disconnected"));
    ethConnected = false;
    break;

  case ARDUINO_EVENT_ETH_STOP:
    Serial.println(F("[ETH] Stopped"));
    ethConnected = false;
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

// Print periodic system status
void printStatus() {
  Serial.println(F("----------------------------------------"));

  if (isTimeValid()) {
    uint64_t timestampMs = getAccurateTimestampMs();
    uint32_t seconds = timestampMs / 1000;
    uint32_t ms = timestampMs % 1000;

    // Get SIV with thread-safe helper
    byte siv = getSatellitesInView();

    Serial.printf("[STATUS] Time: %lu.%03lu | PPS: %lu | SIV: %d\n", seconds,
                  ms, ppsCount, siv);
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
    while (1)
      delay(1000);
  }

  // Initialize GPS via I2C
  Serial.println(F("[INIT] Starting I2C..."));
  Wire.begin(GPS_SDA_PIN, GPS_SCL_PIN);

  Serial.println(F("[INIT] Connecting to GPS module..."));
  if (!myGNSS.begin()) {
    Serial.println(F("[INIT] ERROR: GPS module not detected! Check wiring."));
    while (1)
      delay(1000);
  }
  Serial.println(F("[INIT] GPS module connected"));

  // Configure PPS output on GPS module
  configureTimePulse();

  // Initialize GPS time subsystem (PPS interrupt + sync task)
  initGpsTime(&myGNSS, i2cMutex, PPS_PIN);

  // Initialize Ethernet
  // NTP server task will be started when we get an IP address
  Serial.println(F("[INIT] Starting Ethernet..."));
  WiFi.onEvent(onEthEvent);
  ETH.begin();
  // multicast DNS (mDNS) allows to resolve hostnames to IP addresses without a
  // DNS server
  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("MDNS responder started, listening on %s.local\n", HOSTNAME);
  }

  Serial.println(F("[INIT] Setup complete - waiting for network...\n"));
}

void loop() {
  // Main loop stays lightweight for fast NTP response
  // GPS sync is handled by the GPS_Sync task triggered by PPS

  // Handle PPS interrupt - only log during startup
  if (ppsTriggered) {
    ppsTriggered = false;

    if (!isTimeValid()) {
      Serial.printf("[PPS] #%lu | Waiting for time sync...\n", ppsCount);
    }
  }

  // Print periodic status
  // if (millis() - lastStatusPrint >= STATUS_PRINT_INTERVAL) {
  //   lastStatusPrint = millis();
  //   printStatus();
  // }

  // NTP server runs in its own task on Core 0
  // GPS sync runs in GPS_Sync task on Core 1
  // Main loop handles PPS events and status reporting

  // Minimal delay - just yield to other tasks briefly
  vTaskDelay(1);
}
