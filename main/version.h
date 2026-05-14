/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.7.1: CSRF protection on every state-changing endpoint.
 * One 16-byte random token per boot, double-submit cookie + form/
 * header validation, /api/csrf for CLI tooling. /reboot promoted
 * from GET to POST. 10 CSRF integration tests in test_broker.py.
 * Closes the v0.6.0 'deferred to avoid lockout' debt. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  7
#define FW_VERSION_PATCH  1
#define FW_VERSION        "0.7.1"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
