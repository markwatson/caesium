#include "gps_time.h"

// Volatile variables shared with ISR
volatile uint32_t ppsEpoch = 0;     // Unix epoch at last PPS
volatile uint32_t ppsMicros = 0;    // micros() at last PPS rising edge
volatile bool ppsTriggered = false; // Flag: new PPS received
volatile uint32_t ppsCount = 0;     // Debug: count PPS pulses

// Time validity - set true only after GPS confirms time
volatile bool hasValidTime = false;

// Pending sync - queued during GPS poll, applied at next PPS
volatile uint32_t pendingEpoch = 0;
volatile bool hasPendingSync = false;
volatile bool needsInitialSync = true; // Need sync on first boot

/**
 * PPS Interrupt Service Routine
 * Captures micros() on rising edge of PPS signal.
 * The PPS rising edge marks the exact start of a new second.
 *
 * TIMING: The GPS epoch we receive corresponds to the LAST PPS.
 * So when a pending sync is queued, we apply (pendingEpoch + 1)
 * at the NEXT PPS edge, since one second has elapsed.
 */
void IRAM_ATTR ppsISR() {
  ppsMicros = micros();
  ppsTriggered = true;
  ppsCount++;

  // Apply pending sync from GPS poll (epoch+1 because this is next PPS)
  if (hasPendingSync) {
    ppsEpoch = pendingEpoch + 1;
    hasValidTime = true;
    hasPendingSync = false;
    needsInitialSync = false; // Initial sync complete
  } else if (hasValidTime) {
    // Normal operation: increment epoch on each PPS
    ppsEpoch++;
  }
}

/**
 * Initialize PPS interrupt on specified pin
 */
void initPPS(uint8_t pin) {
  pinMode(pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pin), ppsISR, RISING);
  Serial.printf("[GPS_TIME] PPS interrupt attached to pin %d (RISING edge)\n",
                pin);
}

/**
 * Queue GPS epoch for sync at next PPS edge
 * The epoch from GPS corresponds to the LAST PPS pulse.
 * We queue it and apply (epoch+1) at the NEXT PPS for exact alignment.
 *
 * IMPORTANT: Only queues if we need initial sync or time has become invalid.
 * Once synced, the PPS ISR maintains accurate time by incrementing epoch.
 */
void queueTimeSync(uint32_t epoch) {
  // Only queue if we need initial sync or if time has become invalid
  if (!needsInitialSync && hasValidTime) {
    // Already have valid time, no need to re-sync
    // The ISR is maintaining accurate time via PPS increments
    return;
  }

  noInterrupts();
  pendingEpoch = epoch;
  hasPendingSync = true;
  interrupts();

  Serial.printf(
      "[GPS_TIME] Sync queued: epoch=%lu (will apply %lu at next PPS)\n", epoch,
      epoch + 1);
}

/**
 * Get accurate timestamp in milliseconds
 * Combines the epoch from last PPS with elapsed microseconds since then.
 */
uint64_t getAccurateTimestampMs() {
  // Capture volatile values atomically
  noInterrupts();
  uint32_t capturedEpoch = ppsEpoch;
  uint32_t capturedPpsMicros = ppsMicros;
  interrupts();

  uint32_t currentMicros = micros();

  // Calculate elapsed microseconds since last PPS
  // Handle micros() overflow (wraps every ~70 minutes)
  uint32_t elapsedMicros;
  if (currentMicros >= capturedPpsMicros) {
    elapsedMicros = currentMicros - capturedPpsMicros;
  } else {
    // micros() wrapped around
    elapsedMicros = (0xFFFFFFFF - capturedPpsMicros) + currentMicros + 1;
  }

  // Convert to milliseconds
  // epoch is in seconds, so epoch * 1000 gives ms
  // elapsedMicros / 1000 gives ms since last PPS
  uint64_t timestampMs =
      (uint64_t)capturedEpoch * 1000ULL + (elapsedMicros / 1000);

  return timestampMs;
}

/**
 * Check if time is valid (GPS-synchronized and confirmed)
 */
bool isTimeValid() { return hasValidTime; }
