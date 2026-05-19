# Berry — embedded scripting language.
#
# Upstream sources live in ./upstream/ as a git submodule pinned to a
# berry-lang/berry release tag. Local IDF integration files live alongside:
#
#   ./upstream/        ← submodule (berry-lang/berry @ v1.1.0)
#   ./port/            ← local IDF port (berry_conf.h, be_port.c, be_modtab.c)
#   ./patches/         ← unified diffs applied to ./upstream/ at configure time
#   ./CMakeLists.txt   ← this file
#   ./README.md        ← refresh procedure + local-modification log
#
# See README.md for the upstream-bump workflow.

set(BERRY_DIR    "${CMAKE_CURRENT_LIST_DIR}")
set(BERRY_UP     "${BERRY_DIR}/upstream")
set(BERRY_SRC    "${BERRY_UP}/src")
set(BERRY_PATCH  "${BERRY_DIR}/patches")
set(BERRY_STAMP  "${CMAKE_CURRENT_BINARY_DIR}/berry-patches.stamp")

# Fail loudly if the submodule wasn't checked out.
if(NOT EXISTS "${BERRY_SRC}/be_vm.c")
    message(FATAL_ERROR
        "components/berry/upstream/ is empty. Run:\n"
        "    git submodule update --init --recursive\n"
        "from the repo root, then re-run idf.py build.")
endif()

# Apply local patches once per configure. Patches must be idempotent or the
# stamp file will skip them after the first successful apply.
file(GLOB BERRY_PATCHES "${BERRY_PATCH}/*.patch")
list(SORT BERRY_PATCHES)

set(_need_patch FALSE)
if(NOT EXISTS "${BERRY_STAMP}")
    set(_need_patch TRUE)
else()
    foreach(p ${BERRY_PATCHES})
        if("${p}" IS_NEWER_THAN "${BERRY_STAMP}")
            set(_need_patch TRUE)
            break()
        endif()
    endforeach()
endif()

if(_need_patch AND BERRY_PATCHES)
    # Reset the submodule worktree so re-applying patches is a no-op race-free.
    execute_process(
        COMMAND git -C "${BERRY_UP}" reset --hard --quiet
        RESULT_VARIABLE _rc
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT _rc EQUAL 0)
        message(WARNING "git reset on berry submodule failed (rc=${_rc}); "
                        "patches may stack. Clean with 'git -C ${BERRY_UP} reset --hard'.")
    endif()
    foreach(p ${BERRY_PATCHES})
        message(STATUS "berry: applying ${p}")
        execute_process(
            COMMAND git -C "${BERRY_UP}" apply --whitespace=nowarn "${p}"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "Failed to apply berry patch: ${p}")
        endif()
    endforeach()
    file(WRITE "${BERRY_STAMP}" "ok")
endif()

file(GLOB BERRY_SRCS "${BERRY_SRC}/*.c")
list(APPEND BERRY_SRCS
    "${BERRY_DIR}/port/be_port.c"
    "${BERRY_DIR}/port/be_modtab.c"
)

# Drop OS module — disabled in port/berry_conf.h and depends on dirent/chdir
# that we don't want to pull in.
list(FILTER BERRY_SRCS EXCLUDE REGEX ".*be_oslib\\.c$")
# Drop the upstream standalone interpreter entry point (it has main()).
list(FILTER BERRY_SRCS EXCLUDE REGEX ".*/berry\\.c$")
# Drop be_byteslib: its file-scope be_local_closure blocks reference
# coc-generated be_const_str_* symbols unconditionally and so require
# BE_USE_PRECOMPILED_OBJECT=1. We'll restore this when precompiled-object
# support is wired up (see plan-berry-scripting.md, P2/P3).
list(FILTER BERRY_SRCS EXCLUDE REGEX ".*be_byteslib\\.c$")
# Drop be_filelib: it references be_tobytes/be_pushbytes/be_isbytes from
# be_byteslib. We have no use case for `open()` from Berry scripts on the
# broker yet, so cutting this saves the trouble of stubbing the bytes API.
list(FILTER BERRY_SRCS EXCLUDE REGEX ".*be_filelib\\.c$")

idf_component_register(
    SRCS ${BERRY_SRCS}
    INCLUDE_DIRS "${BERRY_SRC}" "${BERRY_DIR}/port"
    REQUIRES log
)

# Suppress warnings that fire heavily in upstream Berry but are not our bug.
# We don't want to police a third-party codebase; we just want it to build.
component_compile_options(
    -Wno-old-style-declaration
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-sign-compare
    -Wno-format
    -Wno-shadow
    -Wno-discarded-qualifiers
    -Wno-type-limits
    -Wno-error=restrict
    -Wno-restrict
    -Wno-error=stringop-truncation
    -Wno-stringop-truncation
)
