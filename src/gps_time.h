#ifndef GPS_TIME_H
#define GPS_TIME_H

#include <Arduino.h>

// PPS Pin - GPIO36 is input-only on ESP32, perfect for PPS signal
#define PPS_PIN 36

// Volatile variables for ISR - shared between ISR and main code
extern volatile uint32_t ppsEpoch;  // Unix epoch at last PPS (set from GPS)
extern volatile uint32_t ppsMicros; // micros() captured at last PPS rising edge
extern volatile bool ppsTriggered;  // Flag set by ISR, cleared by main loop
extern volatile uint32_t ppsCount;  // Debug counter for PPS pulses

// Time validity flag - only true when GPS time is confirmed
extern volatile bool hasValidTime;

// Pending epoch - set during GPS poll, applied at next PPS
extern volatile uint32_t pendingEpoch; // Epoch to apply at next PPS
extern volatile bool hasPendingSync;   // Flag: pending sync waiting for PPS
extern volatile bool needsInitialSync; // Flag: true until first sync completes

/**
 * @brief PPS Interrupt Service Routine
 * Called on rising edge of PPS signal. Captures micros() timestamp.
 * Must be placed in IRAM for ESP32.
 */
void IRAM_ATTR ppsISR();

/**
 * @brief Initialize the PPS interrupt
 * @param pin GPIO pin connected to PPS signal (default PPS_PIN = 36)
 */
void initPPS(uint8_t pin = PPS_PIN);

/**
 * @brief Queue GPS epoch for sync at next PPS edge
 * Called when GPS reports valid and confirmed time.
 * The epoch will be applied atomically when next PPS arrives.
 * @param epoch Unix epoch seconds from GPS (corresponds to last PPS)
 */
void queueTimeSync(uint32_t epoch);

/**
 * @brief Get accurate timestamp in milliseconds
 * Combines last known epoch with elapsed time since last PPS.
 * @return uint64_t Current timestamp in milliseconds since Unix epoch
 */
uint64_t getAccurateTimestampMs();

/**
 * @brief Check if we have valid GPS-synchronized time
 * @return true if time has been synchronized from GPS with confirmed time
 */
bool isTimeValid();

#endif // GPS_TIME_H
