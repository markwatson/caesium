/**
 * GPS Time Synchronization Module — Push/Callback Architecture
 *
 * How it works:
 * 1. PPS ISR fires on the rising edge (exact second boundary), captures
 *    esp_timer_get_time() and sets a flag. At this point we know a second
 *    started but NOT which second.
 * 2. ~20-80ms later, the GPS finishes transmitting the UBX-NAV-PVT message
 *    over UART. The SparkFun library assembles and checksums it, then calls
 *    pvtCallback(). The PVT data describes the time of the preceding PPS.
 * 3. pvtCallback() checks the PPS flag. If set, it pairs the PPS hardware
 *    timestamp with the GPS epoch and atomically updates timeState.
 * 4. NTP reads timeState + current esp_timer_get_time() to compute exact time.
 */

#include "gps_time.h"

// Validated time state (read by NTP, written by PVT callback)
volatile TimeState timeState = {0, 0, false};
portMUX_TYPE timeStateMux = portMUX_INITIALIZER_UNLOCKED;

// PPS interrupt state (written by ISR, consumed by PVT callback)
// Protected by timeStateMux since ppsTimestampUs is 64-bit (non-atomic on ESP32)
static volatile int64_t ppsTimestampUs = 0;
static volatile bool ppsFlag = false;

// Debug counters
volatile uint32_t ppsCount = 0;
volatile bool ppsTriggered = false;

// Cached SIV from latest PVT message
static volatile uint8_t lastSIV = 0;

// Convert GPS date/time fields directly to Unix epoch.
// Avoids mktime() which depends on the C library's timezone state.
static uint32_t gpsToEpoch(uint16_t year, uint8_t month, uint8_t day,
                           uint8_t hour, uint8_t min, uint8_t sec) {
  // Days from 1970-01-01 to start of each month (non-leap year)
  static const uint16_t monthDays[] = {0,   31,  59,  90,  120, 151,
                                       181, 212, 243, 273, 304, 334};

  // Years since 1970
  uint32_t y = year - 1970;

  // Leap years between 1970 and the start of this year
  // (every 4 years, minus every 100, plus every 400)
  uint32_t leaps = ((year - 1) / 4) - ((year - 1) / 100) + ((year - 1) / 400)
                   - (1969 / 4) + (1969 / 100) - (1969 / 400);

  uint32_t days = y * 365 + leaps + monthDays[month - 1] + (day - 1);

  // Add leap day if current year is leap and we're past February
  if (month > 2 && (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0)) {
    days++;
  }

  return days * 86400UL + hour * 3600UL + min * 60UL + sec;
}

/**
 * PPS Interrupt Service Routine
 *
 * Captures the 64-bit hardware timer (no rollover for ~292,000 years)
 * and sets a flag for the PVT callback to consume.
 */
void IRAM_ATTR ppsISR() {
  int64_t now = esp_timer_get_time();

  portENTER_CRITICAL_ISR(&timeStateMux);
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
  lastSIV = pvtData->numSV;

  // Read PPS state under the same spinlock the ISR writes with
  portENTER_CRITICAL(&timeStateMux);
  bool hasPps = ppsFlag;
  int64_t capturedPpsUs = ppsTimestampUs;
  portEXIT_CRITICAL(&timeStateMux);

  if (!hasPps) {
    return;
  }

  if (!pvtData->valid.bits.validDate || !pvtData->valid.bits.validTime) {
    return;
  }

  uint32_t epoch = gpsToEpoch(pvtData->year, pvtData->month, pvtData->day,
                              pvtData->hour, pvtData->min, pvtData->sec);

  portENTER_CRITICAL(&timeStateMux);
  timeState.epochSec = epoch;
  timeState.ppsTimeMicros = capturedPpsUs;
  timeState.valid = true;
  ppsFlag = false;
  portEXIT_CRITICAL(&timeStateMux);
}

void initGpsTime(uint8_t ppsPin) {
  timeState.epochSec = 0;
  timeState.ppsTimeMicros = 0;
  timeState.valid = false;

  pinMode(ppsPin, INPUT);
  attachInterrupt(digitalPinToInterrupt(ppsPin), ppsISR, RISING);

  Serial.printf("[GPS_TIME] PPS interrupt attached to pin %d\n", ppsPin);
}

bool isTimeValid() { return timeState.valid; }

void getTimeStateAtomic(TimeState &state) {
  portENTER_CRITICAL(&timeStateMux);
  state.epochSec = timeState.epochSec;
  state.ppsTimeMicros = timeState.ppsTimeMicros;
  state.valid = timeState.valid;
  portEXIT_CRITICAL(&timeStateMux);
}

uint64_t getAccurateTimestampMs() {
  TimeState state;
  getTimeStateAtomic(state);

  int64_t nowUs = esp_timer_get_time();
  int64_t elapsedUs = nowUs - state.ppsTimeMicros;
  if (elapsedUs < 0) {
    elapsedUs = 0;
  }

  return (uint64_t)state.epochSec * 1000ULL + (uint64_t)(elapsedUs / 1000);
}

uint8_t getLastSIV() { return lastSIV; }
