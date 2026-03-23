#ifndef GPS_TIME_H
#define GPS_TIME_H

#include <Arduino.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>

// PPS Pin - GPIO16 on ESP32-PoE-ISO
#define PPS_PIN 16

/**
 * Time state for NTP serving
 *
 * Updated atomically by the PVT callback after pairing a PPS pulse with
 * the GPS time message that describes it. Protected by timeStateMux.
 */
struct TimeState {
  uint32_t epochSec;     // Unix epoch at last validated PPS
  int64_t ppsTimeMicros; // esp_timer_get_time() captured at that PPS
  bool valid;            // True once PPS + GPS time have been paired
};

// Time state - shared between PVT callback (Core 1) and NTP callback (Core 0)
extern volatile TimeState timeState;

// Spinlock for atomic access to timeState (works across ESP32 cores)
extern portMUX_TYPE timeStateMux;

// Debug counters (volatile for ISR access)
extern volatile uint32_t ppsCount;
extern volatile bool ppsTriggered;

/**
 * PVT callback - registered with the SparkFun library via setAutoPVTcallbackPtr.
 * Called from checkCallbacks() when a complete UBX-NAV-PVT packet arrives.
 * Pairs the most recent PPS timestamp with the GPS time data.
 */
void pvtCallback(UBX_NAV_PVT_data_t *pvtData);

/**
 * Initialize GPS time subsystem.
 * Attaches PPS interrupt. Call after GPS module is configured.
 */
void initGpsTime(uint8_t ppsPin = PPS_PIN);

/**
 * Check if we have valid GPS-synchronized time.
 */
bool isTimeValid();

/**
 * Get time state atomically for NTP.
 * Briefly disables interrupts for a consistent snapshot.
 */
void getTimeStateAtomic(TimeState &state);

#endif // GPS_TIME_H
