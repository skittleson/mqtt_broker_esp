/*
 * POSIX TZ preset table. Curated for common deployments; ~40 entries.
 * Sourced from IANA tzdata 2025b; DST rules accurate as of build time.
 * Add more as needed — each entry is ~50 bytes of flash/RODATA.
 *
 * Maintenance: when a country changes DST rules (Brazil 2019, Mexico
 * 2022, …) update the affected row here. The device still works without
 * a firmware update — users who hand-type their TZ string in the input
 * field bypass this table entirely.
 */

#include "tz_presets.h"

const tz_preset_t TZ_PRESETS[] = {
    /* -------- North America -------- */
    {"US — Pacific (PT, DST)",      "PST8PDT,M3.2.0,M11.1.0"},
    {"US — Mountain (MT, DST)",     "MST7MDT,M3.2.0,M11.1.0"},
    {"US — Mountain, no DST (AZ)",  "MST7"},
    {"US — Central (CT, DST)",      "CST6CDT,M3.2.0,M11.1.0"},
    {"US — Eastern (ET, DST)",      "EST5EDT,M3.2.0,M11.1.0"},
    {"US — Hawaii (HST)",           "HST10"},
    {"US — Alaska (AKT, DST)",      "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Canada — Atlantic (AT, DST)", "AST4ADT,M3.2.0,M11.1.0"},
    {"Canada — Newfoundland",       "NST3:30NDT,M3.2.0/0:01,M11.1.0/0:01"},
    {"Mexico — Central (no DST)",   "CST6"},

    /* -------- Europe -------- */
    {"UK / Ireland (GMT/BST)",      "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Western Europe (WET/WEST)",   "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Central Europe (CET/CEST)",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Eastern Europe (EET/EEST)",   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Russia — Moscow (MSK)",       "MSK-3"},

    /* -------- Asia / Pacific -------- */
    {"Israel (IST/IDT)",            "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"India (IST)",                 "<+0530>-5:30"},
    {"China / Singapore (CST/SGT)", "CST-8"},
    {"Japan / Korea (JST/KST)",     "JST-9"},
    {"Australia — Western (AWST)",  "AWST-8"},
    {"Australia — Central (ACST/ACDT)", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia — Eastern (AEST/AEDT)", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia — Queensland",      "AEST-10"},
    {"New Zealand (NZST/NZDT)",     "NZST-12NZDT,M9.5.0,M4.1.0/3"},

    /* -------- South America -------- */
    {"Brazil — São Paulo (BRT)",    "<-03>3"},
    {"Argentina (ART)",             "<-03>3"},
    {"Chile — Santiago (CLT/CLST)", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"Colombia / Peru (-05)",       "<-05>5"},

    /* -------- Africa / Middle East -------- */
    {"South Africa (SAST)",         "SAST-2"},
    {"Egypt (EET)",                 "EET-2"},
    {"UAE / Gulf (GST)",            "<+04>-4"},

    /* -------- Fixed offsets (no DST) -------- */
    {"UTC",                         "UTC0"},
    {"UTC-08:00 (fixed PST)",       "UTC8"},
    {"UTC-07:00 (fixed PDT/MST)",   "UTC7"},
    {"UTC-06:00 (fixed CST/MDT)",   "UTC6"},
    {"UTC-05:00 (fixed EST/CDT)",   "UTC5"},
    {"UTC-04:00 (fixed AST/EDT)",   "UTC4"},
    {"UTC+01:00 (fixed CET)",       "UTC-1"},
    {"UTC+08:00 (fixed CST/SGT)",   "UTC-8"},
    {"UTC+10:00 (fixed AEST)",      "UTC-10"},
};

const size_t TZ_PRESETS_COUNT = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);
