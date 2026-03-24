/**
 * GPS Time Synchronization Module — Push/Callback Architecture
 *
 * How it works:
 * 1. PPS ISR fires on the rising edge (exact second boundary), captures
 *    esp_timer_get_time() and sets a flag. It also measures the interval
 *    between consecutive PPS edges for crystal drift calibration.
 * 2. ~20-80ms later, the GPS finishes transmitting the UBX-NAV-PVT message
 *    over UART. The SparkFun library assembles and checksums it, then calls
 *    pvtCallback(). The PVT data describes the time of the preceding PPS.
 * 3. pvtCallback() checks the PPS flag. If set, it pairs the PPS hardware
 *    timestamp with the GPS epoch, smooths the PPS interval measurement,
 *    and atomically updates timeState.
 * 4. NTP reads timeState + current esp_timer_get_time() to compute exact time,
 *    using the calibrated crystal frequency for sub-second interpolation.
 */

#include "gps_time.h"

// Validated time state (read by NTP, written by PVT callback)
volatile TimeState timeState = {0, 0, 1000000, 0, false};
portMUX_TYPE timeStateMux = portMUX_INITIALIZER_UNLOCKED;

// PPS interrupt state (written by ISR, consumed by PVT callback)
// Protected by timeStateMux since int64_t is non-atomic on ESP32
static volatile int64_t ppsTimestampUs = 0;
static volatile bool ppsFlag = false;
static volatile int64_t ppsIntervalUs = 0;
static volatile bool ppsIntervalValid = false;

// Debug counters
volatile uint32_t ppsCount = 0;
volatile bool ppsTriggered = false;

// Set after each successful PPS+PVT sync, consumed by main loop
static volatile bool syncJustCompleted = false;

// Crystal drift EMA state (only accessed by pvtCallback on Core 1)
// Q16 fixed-point for sub-microsecond smoothing precision
static uint64_t smoothedIntervalQ16 = 0;
static uint32_t intervalCount = 0;

// EMA alpha = 1/16 (time constant ~16 seconds, smooths ISR jitter)
#define DRIFT_EMA_SHIFT 4
// Minimum samples before trusting calibration
#define DRIFT_MIN_SAMPLES 4

// Convert GPS date/time fields directly to Unix epoch.
// Avoids mktime() which depends on the C library's timezone state.
static uint32_t gpsToEpoch(uint16_t year, uint8_t month, uint8_t day,
                           uint8_t hour, uint8_t min, uint8_t sec) {
  // Days from 1970-01-01 to start of each month (non-leap year)
  static const uint16_t monthDays[] = {0,   31,  59,  90,  120, 151,
                                       181, 212, 243, 273, 304, 334};

  uint32_t y = year - 1970;

  uint32_t leaps = ((year - 1) / 4) - ((year - 1) / 100) + ((year - 1) / 400)
                   - (1969 / 4) + (1969 / 100) - (1969 / 400);

  uint32_t days = y * 365 + leaps + monthDays[month - 1] + (day - 1);

  if (month > 2 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) {
    days++;
  }

  return days * 86400UL + hour * 3600UL + min * 60UL + sec;
}

/**
 * PPS Interrupt Service Routine
 *
 * Captures the 64-bit hardware timer, computes interval from previous PPS
 * for crystal drift measurement, and sets a flag for pvtCallback.
 */
void IRAM_ATTR ppsISR() {
  int64_t now = esp_timer_get_time();

  portENTER_CRITICAL_ISR(&timeStateMux);
  if (ppsTimestampUs != 0) {
    ppsIntervalUs = now - ppsTimestampUs;
    ppsIntervalValid = true;
  }
  ppsTimestampUs = now;
  ppsFlag = true;
  portEXIT_CRITICAL_ISR(&timeStateMux);

  ppsCount++;
  ppsTriggered = true;
}

/**
 * PVT Callback — called from checkCallbacks() after checkUblox() assembles
 * a complete, checksum-verified UBX-NAV-PVT packet from the UART stream.
 *
 * The u-blox module sends one PVT message per second, immediately after
 * the PPS pulse. The PVT data describes the time of that pulse.
 */
void pvtCallback(UBX_NAV_PVT_data_t *pvtData) {
  // Read PPS state under the same spinlock the ISR writes with
  portENTER_CRITICAL(&timeStateMux);
  bool hasPps = ppsFlag;
  int64_t capturedPpsUs = ppsTimestampUs;
  int64_t capturedInterval = ppsIntervalUs;
  bool hasInterval = ppsIntervalValid;
  portEXIT_CRITICAL(&timeStateMux);

  if (!hasPps) {
    return;
  }

  // Guard: PVT should arrive within ~80ms of PPS. If the PPS timestamp
  // is >500ms old, a second PPS has likely fired and overwritten the
  // original — pairing this PVT with it would be off by a full second.
  int64_t ppsAge = esp_timer_get_time() - capturedPpsUs;
  if (ppsAge > 500000) {
    portENTER_CRITICAL(&timeStateMux);
    ppsFlag = false;
    portEXIT_CRITICAL(&timeStateMux);
    return;
  }

  if (!pvtData->valid.bits.validDate || !pvtData->valid.bits.validTime) {
    return;
  }

  // Crystal drift measurement: smooth the PPS-to-PPS interval
  uint32_t calibratedInterval = 1000000;

  if (hasInterval) {
    // Sanity check: reject intervals outside ±1000 ppm of 1 second
    // (catches missed PPS, double-fire, startup glitches)
    if (capturedInterval > 999000 && capturedInterval < 1001000) {
      uint64_t sampleQ16 = (uint64_t)capturedInterval << 16;

      if (intervalCount == 0) {
        smoothedIntervalQ16 = sampleQ16;
      } else {
        int64_t delta =
            (int64_t)sampleQ16 - (int64_t)smoothedIntervalQ16;
        smoothedIntervalQ16 += delta >> DRIFT_EMA_SHIFT;
      }
      intervalCount++;
    }
  }

  if (intervalCount >= DRIFT_MIN_SAMPLES) {
    calibratedInterval =
        (uint32_t)((smoothedIntervalQ16 + (1 << 15)) >> 16);
  }

  uint32_t epoch = gpsToEpoch(pvtData->year, pvtData->month, pvtData->day,
                              pvtData->hour, pvtData->min, pvtData->sec);

  portENTER_CRITICAL(&timeStateMux);
  timeState.epochSec = epoch;
  timeState.ppsTimeMicros = capturedPpsUs;
  timeState.usPerPps = calibratedInterval;
  timeState.valid = true;
  ppsFlag = false;
  portEXIT_CRITICAL(&timeStateMux);

  syncJustCompleted = true;
}

void initGpsTime(uint8_t ppsPin) {
  timeState.epochSec = 0;
  timeState.ppsTimeMicros = 0;
  timeState.usPerPps = 1000000;
  timeState.leapIndicator = 0;
  timeState.valid = false;

  pinMode(ppsPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(ppsPin), ppsISR, RISING);

  Serial.printf("[GPS_TIME] PPS interrupt attached to pin %d\n", ppsPin);
}

/**
 * Check staleness: if the last PPS+PVT sync is older than the timeout,
 * the time is no longer trustworthy.
 */
static bool isTimeFresh(int64_t ppsTimeMicros) {
  if (ppsTimeMicros == 0) {
    return false;
  }
  int64_t age = esp_timer_get_time() - ppsTimeMicros;
  return age >= 0 && age < GPS_STALE_TIMEOUT_US;
}

bool isTimeValid() {
  return timeState.valid && isTimeFresh(timeState.ppsTimeMicros);
}

void getTimeStateAtomic(TimeState &state) {
  portENTER_CRITICAL(&timeStateMux);
  state.epochSec = timeState.epochSec;
  state.ppsTimeMicros = timeState.ppsTimeMicros;
  state.usPerPps = timeState.usPerPps;
  state.leapIndicator = timeState.leapIndicator;
  state.valid = timeState.valid;
  portEXIT_CRITICAL(&timeStateMux);

  if (state.valid && !isTimeFresh(state.ppsTimeMicros)) {
    state.valid = false;
  }
}

uint32_t getCrystalCalibration() {
  if (intervalCount < DRIFT_MIN_SAMPLES) {
    return 0;
  }
  return (uint32_t)((smoothedIntervalQ16 + (1 << 15)) >> 16);
}

void setLeapIndicator(uint8_t li) {
  portENTER_CRITICAL(&timeStateMux);
  timeState.leapIndicator = li;
  portEXIT_CRITICAL(&timeStateMux);
}

bool consumeSyncEvent() {
  if (syncJustCompleted) {
    syncJustCompleted = false;
    return true;
  }
  return false;
}
