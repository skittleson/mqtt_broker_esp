/*
 * Firmware version — displayed in portal, JSON API, and OTA checks.
 * Follows semantic versioning: MAJOR.MINOR.PATCH
 */

#ifndef VERSION_H
#define VERSION_H

/* 0.8.0: Scheduled MQTT publishes (Tasmota-style "Timers").
 * 16 wall-clock slots stored in NVS "mqtt_cfg"/"timers" as a compact JSON
 * blob (schema v=1). 1Hz scheduler task fires through the existing
 * thread-safe publish queue via new broker_publish_local() API. Honours
 * the POSIX TZ string from "ntp"/"tz" so DST transitions are automatic;
 * spring-forward gaps skip cleanly, fall-back is deduped by the
 * per-slot last-fire-UTC-minute cache. New portal pages /timers and
 * /timers/edit, JSON /api/timers, master pause toggle, per-slot test-fire.
 * No fires until time(NULL) >= 2023-01-01 (SNTP synced). 10-year
 * lifetime preserved: zone rules live in user-editable NVS, not code. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  8
#define FW_VERSION_PATCH  0
#define FW_VERSION        "0.8.0"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
