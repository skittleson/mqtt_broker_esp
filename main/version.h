/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.7.0: NTP feature complete (plan-ntp-server.md Phases 1-3).
 * Tagged release. Test suite (test_ntp.py + test_broker.py) green
 * against the live device on this build; `make test` runs both. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  7
#define FW_VERSION_PATCH  0
#define FW_VERSION        "0.7.0"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
