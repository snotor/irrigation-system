#pragma once
#include <stdint.h>
#include <stddef.h>

// All values are defaults to calibrate on the bench; move them to NVS/settings once the UI exists.

constexpr uint8_t ZONES = 4;

// ---- Relay board ----
// Measured on the bench: this board switches ON with a HIGH input (active-HIGH).
// If the module has an H/L trigger jumper, moving it flips this.
constexpr bool RELAY_ACTIVE_LOW = false;

// ---- Water safety ----
constexpr uint32_t PUMP_START_DELAY_MS  = 1500;       // valve opens, then the pump starts
constexpr uint32_t VALVE_CLOSE_DELAY_MS = 1500;       // pump stops, then the valve closes
constexpr uint32_t MIN_RUN_S            = 60;
constexpr uint32_t MAX_RUN_S            = 60 * 60;    // per run
constexpr uint32_t MAX_ZONE_DAY_S       = 90 * 60;    // per zone per day
constexpr uint32_t HARD_CAP_MARGIN_MS   = 30000;      // each output's own backstop, above MAX_RUN_S

// ---- Tank (HC-SR04 mounted on top, looking down) ----
constexpr float    TANK_LOW_DIST_CM  = 25.0f;         // water surface farther than this = low
constexpr float    TANK_MIN_VALID_CM = 2.0f;
constexpr float    TANK_MAX_VALID_CM = 200.0f;
constexpr uint32_t TANK_PERIOD_MS    = 500;
constexpr uint8_t  TANK_BAD_TO_LOW   = 2;             // consecutive bad readings -> LOW
constexpr uint8_t  TANK_GOOD_TO_OK   = 3;             // consecutive good readings -> OK

// ---- Soil sensors (capacitive: higher raw = drier) ----
// One entry per zone; Hardware.h refuses to build if the count differs from ZONES.
constexpr int SOIL_DRY_RAW[4] = {2800, 2800, 2800, 2800};
constexpr int SOIL_WET_RAW[4] = {1200, 1200, 1200, 1200};
constexpr int SOIL_RAW_MIN_OK = 100;                  // outside min..max = disconnected or shorted
constexpr int SOIL_RAW_MAX_OK = 4000;

// ---- Rain sensor SA-28 (digital, active LOW: see PINS.h) ----
constexpr uint32_t RAIN_DEBOUNCE_MS = 2000;
constexpr uint32_t RAIN_POWERUP_MS  = 300;            // settle time after switching its VCC on

// ---- BME280 ----
constexpr uint32_t ENV_PERIOD_MS   = 5000;
constexpr uint8_t  ENV_FAULT_COUNT = 3;               // consecutive bad reads before it counts as faulted

// ---- Button (pressed = HIGH: 1k pull-down, button to 3.3 V) ----
constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
constexpr uint32_t BUTTON_LONG_MS     = 1500;

// ---- Local time (the RTC stays in UTC; convert only where the schedule is decided) ----
constexpr const char* LOCAL_TZ = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Budapest, DST rules included

// ---- Schedule defaults (all TBD). Settings.h stores the live values in NVS and clamps them to the limits above ----
constexpr uint16_t DEFAULT_WINDOW_START_MIN = 6 * 60;  // 06:00 local
constexpr uint16_t DEFAULT_WINDOW_END_MIN   = 10 * 60; // 10:00 local
constexpr uint16_t DEFAULT_RUN_S            = 300;     // 5 min per zone per day
constexpr int8_t   DEFAULT_ZONE_COLD_C      = 4;       // per-zone cold limit (ambient)
constexpr float    DEFAULT_FROST_C          = 0.0f;    // global frost stop
constexpr uint32_t QUEUE_TIMEOUT_S          = 120 * 60;// a zone waiting longer than this is skipped and logged

// ---- Logging ----
constexpr float  FLOW_LPM      = 0.0f;                 // litres/min of the pump+valve path: measure with bucket and stopwatch; 0 = unknown, litres column stays empty
constexpr size_t LOG_MAX_BYTES = 256 * 1024;           // /log.csv rotates to /log_old.csv above this

// ---- Misc ----
constexpr uint32_t RTC_MIN_VALID_UNIX = 1767225600;   // 2026-01-01 UTC: anything older means the clock lost its time
constexpr uint32_t WDT_TIMEOUT_S      = 10;
