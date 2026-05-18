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
/* 0.8.1: /timers UX fixes (docs/timers-ux-audit-v0.8.0.md).
 * P0: numeric TZ offset on /timers (POSIX %Z gives "UTC" for `UTC7`, was
 * lying); dropped the "24h" claim on the time input (browsers honour
 * locale and show AM/PM in US); empty slot's edit form no longer pre-fills
 * a misleading "12:00 AM". P1: dedicated "Rep" column for once/repeat (was
 * jammed into Time); 3-state On indicator (● armed / ◐ disarmed / — empty);
 * "Next fire" line under the time picker so users can sanity-check their
 * schedule against the device's TZ before leaving the form; trimmed the
 * footer (zero-dropped is the boring case). No flash penalty > 1 KB. */
#define FW_VERSION_MAJOR  0
#define FW_VERSION_MINOR  8
#define FW_VERSION_PATCH  1
#define FW_VERSION        "0.8.1"
#define FW_NAME           "mqtt_broker"

#endif /* VERSION_H */
