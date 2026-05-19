# Berry — submodule layout

Upstream: <https://github.com/berry-lang/berry>
Pinned tag: **v1.1.0** (commit `b5ede66721937533fbf5c286ef44a5111ea30c75`)
Adopted submodule layout: 2026-05-19

## Why a submodule

Berry's source tree is ~1.5 MB of C. We don't want it living in this
repo's git history forever, but we *do* need a small set of local
patches and a custom IDF port. The submodule keeps upstream out of our
history while letting us pin a known-good tag and re-apply local diffs
deterministically at build time.

## Layout

```
components/berry/
  upstream/      ← git submodule → berry-lang/berry @ v1.1.0
  port/          ← local IDF port:
                    berry_conf.h  - feature flags (OS off, no precompiled-obj)
                    be_modtab.c   - trimmed module table
                    be_port.c     - stdio + fs stubs (FatFs/MSVC branches removed)
  patches/       ← local diffs applied to upstream/ at configure time
                    0001-bntvmodule-init.patch
  CMakeLists.txt ← IDF component wrapper; globs upstream/src + port
  README.md      ← this file
```

## First-time clone

```bash
git clone https://github.com/skittleson/mqtt_broker_esp.git
cd mqtt_broker_esp
git submodule update --init --recursive
```

If you forget the submodule update, `idf.py build` aborts with a
descriptive error pointing you at the right command.

## How patching works

`CMakeLists.txt` runs at configure time:

1. Verifies `upstream/src/be_vm.c` exists (submodule is initialized).
2. If the patch stamp is missing or any patch is newer than the stamp:
   `git -C upstream reset --hard` → `git -C upstream apply patches/*.patch`.
3. Writes `<build>/berry-patches.stamp` so subsequent configures skip the
   re-apply unless a patch changes.

Patches are deliberately stored as plain unified diffs (not git
format-patch with hashes) so they survive submodule HEAD changes that
don't touch the file region.

## Local changes vs upstream

### `0001-bntvmodule-init.patch` — `src/berry.h`

v1.1.0 has an inconsistency: `be_define_native_module(_name, _init)`
initializes a `.init` member via designated init, but `struct bntvmodule`
doesn't declare one. GCC 14 rejects this as `'bntvmodule' has no member
named 'init'`. The patch adds `void *init;` at the end of the struct —
the field is never read by Berry's core (module init lookup goes through
Berry-level member names in `be_module.c`), so the field type doesn't
matter; this is purely to satisfy the macro. Tasmota's Berry fork
carries the same fix.

When bumping to v1.1.x or v1.2.x, check whether upstream merged the
fix. If they did, delete the patch file. If not, the patch should still
apply cleanly.

## Refreshing upstream

```bash
# 1. Bump the submodule to a new tag.
git -C components/berry/upstream fetch --tags origin
git -C components/berry/upstream checkout v<X.Y.Z>
git add components/berry/upstream

# 2. Re-vet port/berry_conf.h against upstream/default/berry_conf.h
#    for any new feature flags introduced upstream.
diff -u components/berry/port/berry_conf.h \
        components/berry/upstream/default/berry_conf.h | less

# 3. Try the build. If patches no longer apply cleanly, regenerate:
idf.py build
# If a patch fails:
#   - cd components/berry/upstream
#   - hand-apply the change against the new sources
#   - git diff > ../patches/0001-foo.patch
#   - rm <build>/esp-idf/berry/berry-patches.stamp ; idf.py build

# 4. Bump the version + commit-hash header at the top of this README.
```

## Build notes

A few upstream sources are excluded from the IDF build via CMake
`list(FILTER ...)` rules in `CMakeLists.txt`:

- `be_oslib.c` — depends on `dirent.h`/`chdir`; OS module is off in `berry_conf.h`
- `berry.c` — standalone interpreter `main()`
- `be_byteslib.c` — requires `BE_USE_PRECOMPILED_OBJECT=1` (we don't ship the host `coc` tool)
- `be_filelib.c` — depends on `be_byteslib.c`; no use case yet

If a future bump rearranges these into a different file layout, update
the filter list. Re-enabling `be_byteslib.c` is tracked in
`plan-berry-scripting.md` (phase P2/P3) and requires wiring up the
upstream `coc` precompiled-object generator.
