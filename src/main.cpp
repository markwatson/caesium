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
 * - Core 0: NTP server task (networking)
 * - Core 1: Main loop drives checkUblox() to process UART and trigger callbacks
 */

#include <ESPmDNS.h>
#include <ETH.h>

#include "SparkFun_u-blox_GNSS_Arduino_Library.h"
#include "gps_time.h"
#include "ntp.h"

// Hostname used for both ETH and mDNS
const char HOSTNAME[] = "caesium";

// GPS module instance
SFE_UBLOX_GNSS myGNSS;

// UART pins for GNSS module (ESP32-PoE-ISO Serial1)
// TX1=GPIO4 (ESP32 TX -> GNSS RX), RX1=GPIO36 (GNSS TX -> ESP32 RX)
#define GPS_TX_PIN 4
#define GPS_RX_PIN 36
#define GPS_BAUD 38400

// Task handle for NTP server
static TaskHandle_t ntpTaskHandle = NULL;

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

  // Initialize Ethernet
  Serial.println(F("[INIT] Starting Ethernet..."));
  Network.onEvent(onEthEvent);
  ETH.begin();

  if (MDNS.begin(HOSTNAME)) {
    Serial.printf("MDNS responder started, listening on %s.local\n", HOSTNAME);
  }

  Serial.println(F("[INIT] Setup complete\n"));
}

void loop() {
  // Drive the SparkFun library: read UART bytes and assemble packets,
  // then fire any pending callbacks (e.g. pvtCallback).
  myGNSS.checkUblox();
  myGNSS.checkCallbacks();

  // Log PPS during startup (before time is valid)
  if (ppsTriggered) {
    ppsTriggered = false;

    if (!isTimeValid()) {
      Serial.printf("[PPS] #%lu | Waiting for time sync...\n", ppsCount);
    }
  }

  // Yield briefly so NTP task and other FreeRTOS tasks can run
  vTaskDelay(1);
}
