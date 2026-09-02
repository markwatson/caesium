#ifndef GPS_TIME_H
#define GPS_TIME_H

#include <Arduino.h>
#include <SparkFun_u-blox_GNSS_Arduino_Library.h>

// PPS Pin - GPIO16 on ESP32-PoE-ISO
#define PPS_PIN 16

// If no PPS+PVT sync for this many microseconds, report unsynchronized.
// At 20-40 ppm crystal drift, 5 seconds accumulates ~100-200µs of error.
#define GPS_STALE_TIMEOUT_US 5000000

/**
 * Time state for NTP serving
 *
 * Updated atomically by the PVT callback after pairing a PPS pulse with
 * the GPS time message that describes it. Protected by timeStateMux.
 */
struct TimeState {
  uint32_t epochSec;     // Unix epoch at last validated PPS
  int64_t ppsTimeMicros; // esp_timer_get_time() captured at that PPS
  uint32_t usPerPps;     // Measured crystal ticks per PPS interval (smoothed)
  uint8_t leapIndicator; // NTP LI value: 0=none, 1=insert, 2=delete
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

/**
 * Get the current crystal drift measurement for debug logging.
 * Returns smoothed microseconds-per-PPS-interval, or 0 if not yet calibrated.
 */
uint32_t getCrystalCalibration();

/**
 * Set the NTP leap indicator (called from main.cpp after polling GPS).
 * Values: 0=no warning, 1=last minute has 61s, 2=last minute has 59s.
 */
void setLeapIndicator(uint8_t li);

/**
 * Returns true once after each successful PPS+PVT sync.
 * Used by main loop to know the UART is idle and safe for additional polls.
 */
bool consumeSyncEvent();

/**
 * Open a UART service cycle. Call at the top of every loop iteration, before
 * checkUblox/checkCallbacks. Records the PPS edge count and the time since the
 * last cycle; pvtCallback uses both to reject a PVT that belongs to a
 * different second than the PPS timestamp it would be paired with.
 */
void beginUartCycle();

/**
 * Number of PVT publishes dropped because the PPS/PVT pairing could not be
 * trusted. Non-zero means this device was starved across a second boundary;
 * a steadily climbing value is worth investigating.
 */
uint32_t getDroppedPairingCount();

#endif // GPS_TIME_H
