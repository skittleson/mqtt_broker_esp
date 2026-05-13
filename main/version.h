/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.7.0-rc1: Phase 1 of plan-ntp-server.md -- SNTP client only. Phase 2
 * (SNTP server) lands separately. Bumped minor to flag the new feature
 * boundary even though the rc1 build is purely additive. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  7
#define FW_VERSION_PATCH  0
#define FW_VERSION        "0.7.0-rc1"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
