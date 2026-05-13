/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.7.0-rc2: Phase 2 of plan-ntp-server.md -- SNTP server lands. UDP :123
 * answers SNTPv4 with stratum=16/LI=3 (alarm) while unsynced and stratum 3
 * once we have time. Phase 3 (DHCP option 42, /time portal page) still
 * pending. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  7
#define FW_VERSION_PATCH  0
#define FW_VERSION        "0.7.0-rc2"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
