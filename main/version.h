/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.7.0-rc3: Phase 3 of plan-ntp-server.md -- /time portal page, mDNS
 * _ntp._udp advertisement (substituting for DHCP option 42 which IDF's
 * DHCP server doesn't expose for arbitrary option codes), and a
 * recent-clients table sourced from the rate-limit LRU. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  7
#define FW_VERSION_PATCH  0
#define FW_VERSION        "0.7.0-rc3"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
