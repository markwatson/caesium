#ifndef GPS_TIME_H
#define GPS_TIME_H

#include <Arduino.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>

// PPS Pin - GPIO36 is input-only on ESP32, perfect for PPS signal
#define PPS_PIN 36

// Configuration
#define GPS_SYNC_INTERVAL_PPS 60    // Sync with GPS every N PPS pulses
#define GPS_SYNC_DELAY_MS 100       // Delay after PPS before polling GPS
#define GPS_STARTUP_SYNC_INTERVAL 2 // Sync every N PPS during startup

/**
 * Time state for NTP serving
 * Protected by timeStateMux spinlock for cross-core safety.
 * Use getTimeStateAtomic() to read, or acquire the spinlock directly.
 */
struct TimeState {
  uint32_t epochSec;      // Unix epoch at last PPS
  uint32_t ppsTimeMicros; // micros() captured at last PPS rising edge
  bool valid;             // True when GPS-synchronized
};

// Time state - shared between ISR and tasks across both cores
extern volatile TimeState timeState;

// Spinlock for atomic access to timeState (works across ESP32 cores)
extern portMUX_TYPE timeStateMux;

// Debug counters (volatile for ISR access)
extern volatile uint32_t ppsCount; // Total PPS pulses received
extern volatile bool ppsTriggered; // Flag for main loop debug logging

// Legacy compatibility accessors (use these in main.cpp for debug logging)
inline uint32_t getPpsEpoch() { return timeState.epochSec; }
inline uint32_t getPpsMicros() { return timeState.ppsTimeMicros; }

// Macros for legacy code that reads these directly
#define ppsEpoch (timeState.epochSec)

/**
 * @brief Initialize GPS time subsystem
 * Sets up PPS interrupt and creates GPS sync task.
 *
 * @param gnss Pointer to initialized GNSS object
 * @param i2cMutex Mutex for I2C access (shared with other I2C users)
 * @param ppsPin GPIO pin connected to PPS signal
 */
void initGpsTime(SFE_UBLOX_GNSS *gnss, SemaphoreHandle_t i2cMutex,
                 uint8_t ppsPin = PPS_PIN);

/**
 * @brief Check if we have valid GPS-synchronized time
 * @return true if time has been synchronized from GPS
 */
bool isTimeValid();

/**
 * @brief Get accurate timestamp in milliseconds
 * Combines last known epoch with elapsed time since last PPS.
 * @return uint64_t Current timestamp in milliseconds since Unix epoch
 */
uint64_t getAccurateTimestampMs();

/**
 * @brief Get time state atomically for NTP
 * Disables interrupts briefly to get consistent snapshot.
 * @param state Output: copy of current time state
 */
void getTimeStateAtomic(TimeState &state);

/**
 * @brief Get satellites in view (thread-safe)
 * @return Number of satellites, or 0 if unavailable
 */
uint8_t getSatellitesInView();

// Legacy API - kept for compatibility during refactor
void initPPS(uint8_t pin = PPS_PIN);

#endif // GPS_TIME_H
