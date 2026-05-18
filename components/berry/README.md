# Berry — vendored

Upstream: https://github.com/berry-lang/berry
Pinned tag: **v1.1.0** (commit b5ede66721937533fbf5c286ef44a5111ea30c75)
Imported: 2026-05-18

## Layout

- `src/*.c`, `src/*.h` — pristine upstream sources from `berry/src/`.
- `port/berry_conf.h` — local config override. Disables OS module,
  precompiled-object generator (no host coc tool needed), bytecode
  saver/loader, and shared-lib loader to stay lean on the ESP32-S3.
- `port/be_modtab.c` — module table; trimmed copy of `berry/default/be_modtab.c`.
- `port/be_port.c` — stdio + filesystem stubs; trimmed copy of
  `berry/default/be_port.c` with FatFs/MSVC branches removed.
- `CMakeLists.txt` — ESP-IDF component wrapper.

## Local changes vs upstream

### `src/berry.h` — add `bntvmodule.init` field

v1.1.0 has an inconsistency: `be_define_native_module(_name, _init)`
initializes a `.init` member via designated init, but the `bntvmodule`
struct doesn't declare one. GCC 14 rejects this as `'bntvmodule' has
no member named 'init'`. We added a `void *init;` field at the end of
the struct — it's never read by Berry's core (module init lookup goes
through Berry-level member names in `be_module.c`), so the field type
doesn't matter; this is purely to satisfy the macro. Tasmota's Berry
fork carries the same fix.

When refreshing upstream, re-apply if v1.1.x still has this issue.

## Refreshing upstream

```bash
git clone --depth 1 --branch v<X.Y.Z> https://github.com/berry-lang/berry.git /tmp/berry-src
rm components/berry/src/*.{c,h}
cp /tmp/berry-src/src/*.c /tmp/berry-src/src/*.h components/berry/src/
# Re-vet port/berry_conf.h against /tmp/berry-src/default/berry_conf.h for new knobs.
idf.py build
```
