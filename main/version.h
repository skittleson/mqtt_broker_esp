/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.7.2: NTP drift compensation while free-running.
 * 8-slot ring of (mono_us, wall_us) sync pairs, linear ppm fit over
 * the longest baseline. Applied to SNTP server tx_timestamp and
 * $SYS/broker/time only when free-running > 2*poll. Plan target:
 * 24h drift 2s -> 200ms. New: $SYS/broker/ntp/drift_ppm and
 * /free_running_s retained topics, drift_ppm + free_running_s in
 * /api/time, drift suffix on /time page server status line. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  7
#define FW_VERSION_PATCH  2
#define FW_VERSION        "0.7.2"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
