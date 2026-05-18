/* mqtt_broker_esp local override for Berry's berry_conf.h.
 *
 * Only the macros we deviate from upstream defaults are defined here.
 * Everything else falls through to src/be_object.h's built-in defaults
 * (we intentionally do NOT #include the upstream default/berry_conf.h
 * to keep the surface obvious).
 */
#ifndef BERRY_CONF_H
#define BERRY_CONF_H

#include <assert.h>

/* --- Integer + float types --- */
#define BE_INTGER_TYPE                  2   /* long long */
#define BE_USE_SINGLE_FLOAT             1   /* single-precision float; saves ~3 KB code + RAM */

/* --- Precompiled objects ---
 * Disabled: would require running Berry's `coc` host tool at build time to
 * pre-generate be_fixed_*.h files. Skipping that for now; we pay a small
 * runtime RAM cost in exchange for build-system simplicity.
 */
#define BE_USE_PRECOMPILED_OBJECT       0

/* --- Debug + perf knobs --- */
#define BE_DEBUG                        0
#define BE_DEBUG_RUNTIME_INFO           1
#define BE_DEBUG_VAR_INFO               1
#define BE_USE_PERF_COUNTERS            1   /* needed for be_set_obs_hook + budget enforcement */
#define BE_VM_OBSERVABILITY_SAMPLING    14  /* 2^14 = ~16k instructions between hook calls */
#define BE_USE_DEBUG_HOOK               1
#define BE_USE_DEBUG_GC                 0
#define BE_USE_DEBUG_STACK              0

/* --- Stack sizing --- */
#define BE_STACK_TOTAL_MAX              4000
#define BE_STACK_FREE_MIN               10
#define BE_STACK_START                  50
#define BE_CONST_SEARCH_SIZE            50
#define BE_USE_STR_HASH_CACHE           0

/* --- File system + bytecode I/O ---
 * Keep file class compiled (it's small) so scripts can read text from
 * VFS-mounted paths if we ever add one. Bytecode persistence + shared
 * library loader are off until we have a real reason.
 */
#define BE_USE_FILE_SYSTEM              1
#define BE_USE_SCRIPT_COMPILER          1
#define BE_USE_BYTECODE_SAVER           0
#define BE_USE_BYTECODE_LOADER          0
#define BE_USE_SHARED_LIB               0
#define BE_USE_OVERLOAD_HASH            1

/* --- Module table ---
 * OS module is excluded: needs FatFs/POSIX dirent which we'd have to
 * port. Everything else is on; the broker's own modules (mqtt, http,
 * watchdog, kv, log) are wired in via port/be_modtab.c.
 */
#define BE_USE_STRING_MODULE            1
#define BE_USE_JSON_MODULE              1
#define BE_USE_MATH_MODULE              1
#define BE_USE_TIME_MODULE              1
#define BE_USE_OS_MODULE                0
#define BE_USE_GLOBAL_MODULE            1
#define BE_USE_SYS_MODULE               1
#define BE_USE_DEBUG_MODULE             1
#define BE_USE_GC_MODULE                1
#define BE_USE_SOLIDIFY_MODULE          0
#define BE_USE_INTROSPECT_MODULE        1
#define BE_USE_STRICT_MODULE            1

/* --- Allocator hooks ---
 * Default to libc malloc/free/realloc; berry_runtime.c will wrap these
 * in a heap-accounting layer once P7 (budget + heap cap) lands.
 */
#define BE_EXPLICIT_ABORT               abort
#define BE_EXPLICIT_EXIT                exit
#define BE_EXPLICIT_MALLOC              malloc
#define BE_EXPLICIT_FREE                free
#define BE_EXPLICIT_REALLOC             realloc

#define be_assert(expr)                 assert(expr)

#endif /* BERRY_CONF_H */
