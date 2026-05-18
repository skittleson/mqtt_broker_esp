# Berry — embedded scripting language, vendored from berry-lang/berry v1.1.0.
# See components/berry/README.md for upstream refresh procedure.

file(GLOB BERRY_SRCS "src/*.c")
list(APPEND BERRY_SRCS
    "port/be_port.c"
    "port/be_modtab.c"
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
    INCLUDE_DIRS "src" "port"
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
