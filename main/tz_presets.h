/*
 * POSIX TZ preset table for the /settings dropdown.
 *
 * Each entry has a human-readable label and the POSIX TZ string newlib
 * understands. The user can still hand-type the field on /settings; the
 * dropdown just makes common zones one-click. Strings are regenerated
 * from IANA tzdata at firmware build time (manually, until we wire
 * tools/gen_tz_presets.py into CI per plan-scheduled-publishes.md §2a).
 */

#ifndef TZ_PRESETS_H
#define TZ_PRESETS_H

#include <stddef.h>

typedef struct {
    const char *label;   /* shown in the dropdown */
    const char *posix;   /* value written to NVS "ntp"/"tz" */
} tz_preset_t;

extern const tz_preset_t TZ_PRESETS[];
extern const size_t      TZ_PRESETS_COUNT;

#endif /* TZ_PRESETS_H */
