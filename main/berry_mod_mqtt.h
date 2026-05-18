/* berry_mod_mqtt.h — Phase 3 `mqtt` native module.
 *
 * Exposes:
 *   mqtt.subscribe(filter, fn)   — register a callback for matching topics
 *   mqtt.unsubscribe(filter)     — remove entries matching filter
 *   mqtt.publish(topic, payload [, qos=0 [, retain=false]])
 *
 * Registration is done at VM construction time via berry_mod_mqtt_register().
 * The module is stored as the global `mqtt` so scripts use it directly
 * (no `import` statement required).
 */
#ifndef BERRY_MOD_MQTT_H
#define BERRY_MOD_MQTT_H

#include "berry.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the mqtt module into the given VM. Called by berry_runtime.c
 * immediately after vm_construct() creates the VM. */
void berry_mod_mqtt_register(bvm *vm);

/* berry_runtime.c port hook — writes to the log ring buffer.
 * Forward-declared here to avoid a circular include with berry_runtime.h. */
void berry_port_stdout_write(const char *buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* BERRY_MOD_MQTT_H */
