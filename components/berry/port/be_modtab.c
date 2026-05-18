/* mqtt_broker_esp module table.
 *
 * Mirrors berry/default/be_modtab.c, minus the OS module (we disable
 * BE_USE_OS_MODULE in port/berry_conf.h to avoid pulling in dirent + chdir).
 *
 * P3+ will register the broker's native modules (mqtt, http, watchdog,
 * kv, log) by adding them in the "user-defined modules" sections below.
 */
#include "berry.h"

/* default modules declare */
be_extern_native_module(string);
be_extern_native_module(json);
be_extern_native_module(math);
be_extern_native_module(time);
be_extern_native_module(global);
be_extern_native_module(sys);
be_extern_native_module(debug);
be_extern_native_module(gc);
be_extern_native_module(introspect);
be_extern_native_module(strict);
be_extern_native_module(undefined);

/* user-defined modules declare start */
/* (P3+) be_extern_native_module(mqtt);    -- registered dynamically by berry_runtime.c */
/* (P4)  be_extern_native_module(http);    */
/* (P6)  be_extern_native_module(watchdog);*/
/* (P6)  be_extern_native_module(kv);      */
/* user-defined modules declare end */

/* module list declaration */
BERRY_LOCAL const bntvmodule* const be_module_table[] = {
#if BE_USE_STRING_MODULE
    &be_native_module(string),
#endif
#if BE_USE_JSON_MODULE
    &be_native_module(json),
#endif
#if BE_USE_MATH_MODULE
    &be_native_module(math),
#endif
#if BE_USE_TIME_MODULE
    &be_native_module(time),
#endif
#if BE_USE_GLOBAL_MODULE
    &be_native_module(global),
#endif
#if BE_USE_SYS_MODULE
    &be_native_module(sys),
#endif
#if BE_USE_DEBUG_MODULE
    &be_native_module(debug),
#endif
#if BE_USE_GC_MODULE
    &be_native_module(gc),
#endif
#if BE_USE_INTROSPECT_MODULE
    &be_native_module(introspect),
#endif
#if BE_USE_STRICT_MODULE
    &be_native_module(strict),
#endif
    &be_native_module(undefined),
    /* user-defined modules register start */
    /* mqtt is registered dynamically by berry_runtime.c::vm_construct() */
    /* user-defined modules register end */
    NULL /* do not remove */
};

/* user-defined classes — none yet */
BERRY_LOCAL bclass_array be_class_table = {
    NULL, /* do not remove */
};

/* Stub for be_load_byteslib(): we exclude be_byteslib.c from the build
 * (it needs BE_USE_PRECOMPILED_OBJECT=1) but be_libs.c calls this
 * unconditionally. Empty stub registers no class; scripts that try
 * to use `bytes` will get a NameError until we wire up precompiled
 * objects in a later phase. */
void be_load_byteslib(bvm *vm)
{
    (void)vm;
}

/* Stub for be_load_filelib(): be_filelib.c is excluded from the build
 * because it references be_tobytes/be_pushbytes/be_isbytes from the
 * bytes class which we also dropped. Scripts that call open() will
 * see a NameError until file I/O + bytes are re-enabled. */
void be_load_filelib(bvm *vm)
{
    (void)vm;
}
