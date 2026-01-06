/**
 * GPS Time Synchronization Module
 *
 * Architecture:
 * - PPS ISR: Fires on rising edge, captures micros(), increments epoch (if
 * synced), notifies sync task periodically via vTaskNotifyGiveFromISR()
 * - Sync Task: Waits for notification, delays 200ms for GPS to update
 * registers, polls GPS via I2C, validates/sets epoch
 * - NTP Handler: Reads {epoch, ppsMicros} atomically, computes current time
 *
 * The key insight is that GPS getUnixEpoch() returns the current time at the
 * moment of the I2C query. By waiting ~200ms after PPS, we're solidly in the
 * middle of that second, and the GPS epoch should match our ppsEpoch.
 */

#include "gps_time.h"

// Time state - the core data structure for NTP
volatile TimeState timeState = {0, 0, false};

// Spinlock for cross-core atomic access to timeState
portMUX_TYPE timeStateMux = portMUX_INITIALIZER_UNLOCKED;

// Debug counters
volatile uint32_t ppsCount = 0;
volatile bool ppsTriggered = false;

// Internal state
static volatile bool needsInitialSync = true;  // True until first GPS sync
static volatile uint32_t ppsSinceLastSync = 0; // Counter for sync interval

// Task handle for GPS sync task (needs to be accessible from ISR)
static TaskHandle_t gpsSyncTaskHandle = NULL;

// GPS module reference and I2C mutex (set by initGpsTime)
static SFE_UBLOX_GNSS *gpsModule = NULL;
static SemaphoreHandle_t gpsI2cMutex = NULL;

/**
 * PPS Interrupt Service Routine
 *
 * Called on rising edge of PPS signal (exact second boundary).
 * - Captures micros() for sub-second timing
 * - Increments epoch if we have valid time
 * - Notifies sync task periodically to validate time
 */
void IRAM_ATTR ppsISR() {
  // Update time state atomically (cross-core safe)
  portENTER_CRITICAL_ISR(&timeStateMux);
  timeState.ppsTimeMicros = micros();
  if (timeState.valid) {
    timeState.epochSec++;
  }
  portEXIT_CRITICAL_ISR(&timeStateMux);

  // These don't need the lock (only accessed by ISR or non-critical)
  ppsCount++;
  ppsSinceLastSync++;
  ppsTriggered = true;

  // Determine if we should trigger a GPS sync
  bool shouldSync = false;

  if (needsInitialSync) {
    // During startup, sync frequently until we get first lock
    if (ppsSinceLastSync >= GPS_STARTUP_SYNC_INTERVAL) {
      shouldSync = true;
    }
  } else {
    // Normal operation: sync periodically to validate
    if (ppsSinceLastSync >= GPS_SYNC_INTERVAL_PPS) {
      shouldSync = true;
    }
  }

  // Notify sync task if needed
  if (shouldSync && gpsSyncTaskHandle != NULL) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(gpsSyncTaskHandle, &xHigherPriorityTaskWoken);
    ppsSinceLastSync = 0;
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

/**
 * GPS Sync Task
 *
 * Waits for notification from PPS ISR, then:
 * 1. Delays to let GPS update its registers
 * 2. Polls GPS for current time via I2C
 * 3. Validates that GPS epoch matches our ppsEpoch (or sets it on first sync)
 */
static void gpsSyncTask(void *parameter) {
  Serial.println(F("[GPS_SYNC] Task started"));

  for (;;) {
    // Wait indefinitely for notification from PPS ISR
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    uint32_t syncStartMs = millis();

    // Capture PPS count immediately after waking to detect all pulses during
    // sync
    uint32_t preQueryPpsCount = ppsCount;

    // Delay to let GPS update registers after PPS
    // GPS internal time updates shortly after PPS pulse
    vTaskDelay(pdMS_TO_TICKS(GPS_SYNC_DELAY_MS));

    // Capture local epoch BEFORE slow I2C operations
    portENTER_CRITICAL(&timeStateMux);
    uint32_t preQueryEpoch = timeState.epochSec;
    bool currentValid = timeState.valid;
    portEXIT_CRITICAL(&timeStateMux);

    // Take I2C mutex
    if (xSemaphoreTake(gpsI2cMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
      Serial.println(F("[GPS_SYNC] Could not acquire I2C mutex"));
      continue;
    }

    // Poll GPS for current time
    uint32_t gpsUs;
    uint32_t gpsEpoch = gpsModule->getUnixEpoch(gpsUs);

    // Check if getUnixEpoch failed (returns 0 on timeout or error)
    if (gpsEpoch == 0) {
      xSemaphoreGive(gpsI2cMutex);
      Serial.println(F("[GPS_SYNC] getUnixEpoch timeout or error, skipping"));
      continue;
    }

    bool timeValid = gpsModule->getTimeValid();
    bool timeConfirmed = gpsModule->getConfirmedTime();
    uint8_t siv = gpsModule->getSIV();

    // Release I2C mutex immediately
    xSemaphoreGive(gpsI2cMutex);

    // Only proceed if GPS reports valid and confirmed time
    if (!timeValid || !timeConfirmed) {
      Serial.printf("[GPS_SYNC] Waiting for GPS lock (SIV: %d)\\n", siv);
      continue;
    }

    // Use pre-query epoch for comparison (from same time window as GPS query
    // start)
    uint32_t currentEpoch = preQueryEpoch;

    if (needsInitialSync || !currentValid) {
      // Initial sync: set epoch directly from GPS
      // But account for PPS pulses that fired during I2C query
      // GPS epoch reflects the time when query started, but we may have
      // crossed into the next second(s) during the slow I2C operation

      // Check how many PPS pulses occurred during the I2C query
      uint32_t postQueryPpsCount = ppsCount;
      uint32_t ppsElapsed = postQueryPpsCount - preQueryPpsCount;

      portENTER_CRITICAL(&timeStateMux);
      // Adjust GPS epoch by elapsed PPS pulses (accounts for I2C latency)
      timeState.epochSec = gpsEpoch + ppsElapsed;
      timeState.valid = true;
      portEXIT_CRITICAL(&timeStateMux);

      needsInitialSync = false;
      uint32_t elapsed = millis() - syncStartMs;
      Serial.printf("[GPS_SYNC] Initial sync: epoch=%lu+%lu, SIV=%d (%lums)\n",
                    gpsEpoch, ppsElapsed, siv, elapsed);
    } else {
      // Validation sync: check that GPS agrees with our epoch
      // Compare against preQueryEpoch - this is the epoch at start of I2C query
      // GPS returns the second we were in when we started the query
      int32_t drift = (int32_t)gpsEpoch - (int32_t)currentEpoch;

      if (drift == 0) {
        // Perfect - GPS confirms our time (only log occasionally)
        uint32_t elapsed = millis() - syncStartMs;
        Serial.printf("[GPS_SYNC] OK: epoch=%lu, SIV=%d (%lums)\n",
                      currentEpoch, siv, elapsed);
      } else {
        // Drift detected - correct it
        // Account for any PPS pulses that fired during I2C query
        portENTER_CRITICAL(&timeStateMux);
        uint32_t currentActualEpoch = timeState.epochSec;
        // Apply the same correction relative to current epoch
        timeState.epochSec = currentActualEpoch + drift;
        portEXIT_CRITICAL(&timeStateMux);

        uint32_t elapsed = millis() - syncStartMs;
        Serial.printf("[GPS_SYNC] Drift corrected: %+ld sec (was %lu, now "
                      "%lu) (%lums)\n",
                      drift, currentActualEpoch, currentActualEpoch + drift,
                      elapsed);
      }
    }
  }
}

/**
 * Initialize GPS time subsystem
 */
void initGpsTime(SFE_UBLOX_GNSS *gnss, SemaphoreHandle_t i2cMutex,
                 uint8_t ppsPin) {
  // Store references
  gpsModule = gnss;
  gpsI2cMutex = i2cMutex;

  // Initialize time state
  timeState.epochSec = 0;
  timeState.ppsTimeMicros = 0;
  timeState.valid = false;
  needsInitialSync = true;
  ppsSinceLastSync = 0;

  // Create GPS sync task BEFORE attaching interrupt
  // Priority 2 = same as NTP task, higher than main loop
  xTaskCreatePinnedToCore(gpsSyncTask,        // Task function
                          "GPS_Sync",         // Task name
                          4096,               // Stack size
                          NULL,               // Parameters
                          2,                  // Priority
                          &gpsSyncTaskHandle, // Task handle
                          1                   // Core 1 (application core)
  );

  // Attach PPS interrupt
  pinMode(ppsPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(ppsPin), ppsISR, RISING);

  Serial.printf(
      "[GPS_TIME] Initialized: PPS on pin %d, sync every %d seconds\n", ppsPin,
      GPS_SYNC_INTERVAL_PPS);
}

/**
 * Legacy init function for compatibility
 */
void initPPS(uint8_t pin) {
  // Just attach interrupt - for use when initGpsTime() handles the rest
  pinMode(pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(pin), ppsISR, RISING);
  Serial.printf("[GPS_TIME] PPS interrupt attached to pin %d (legacy mode)\n",
                pin);
}

/**
 * Get time state atomically
 */
void getTimeStateAtomic(TimeState &state) {
  portENTER_CRITICAL(&timeStateMux);
  state.epochSec = timeState.epochSec;
  state.ppsTimeMicros = timeState.ppsTimeMicros;
  state.valid = timeState.valid;
  portEXIT_CRITICAL(&timeStateMux);
}

/**
 * Check if time is valid
 */
bool isTimeValid() { return timeState.valid; }

/**
 * Get accurate timestamp in milliseconds
 */
uint64_t getAccurateTimestampMs() {
  TimeState state;
  getTimeStateAtomic(state);

  uint32_t currentMicros = micros();

  // Calculate elapsed microseconds since last PPS
  uint32_t elapsedMicros;
  if (currentMicros >= state.ppsTimeMicros) {
    elapsedMicros = currentMicros - state.ppsTimeMicros;
  } else {
    // micros() wrapped around
    elapsedMicros = (0xFFFFFFFF - state.ppsTimeMicros) + currentMicros + 1;
  }

  // Convert to milliseconds
  uint64_t timestampMs =
      (uint64_t)state.epochSec * 1000ULL + (elapsedMicros / 1000);

  return timestampMs;
}

/**
 * Get satellites in view (thread-safe)
 */
uint8_t getSatellitesInView() {
  if (gpsModule == NULL || gpsI2cMutex == NULL) {
    return 0;
  }

  if (xSemaphoreTake(gpsI2cMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }

  uint8_t siv = gpsModule->getSIV();
  xSemaphoreGive(gpsI2cMutex);

  return siv;
}
