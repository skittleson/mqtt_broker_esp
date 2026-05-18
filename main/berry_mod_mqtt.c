/* berry_mod_mqtt.c — Native `mqtt` module for the Berry scripting runtime.
 *
 * Phase 3: mqtt.subscribe(topic_filter, fn) + mqtt.publish(topic, payload, qos, retain)
 *           + mqtt.unsubscribe(topic_filter)
 *
 * Registration:
 *   berry_mod_mqtt_register(vm) is called from berry_runtime.c::vm_construct()
 *   immediately after the VM is constructed. It builds a Berry module object
 *   and stores it as the global `mqtt` so scripts can call:
 *     mqtt.subscribe("sensor/+", def(t,p) print(t,p) end)
 *     mqtt.publish("my/topic", "hello")
 *     mqtt.unsubscribe("sensor/+")
 *
 *   Because we register it as a global rather than via be_module_table[] (which
 *   lives in the `berry` component and can't reference main's symbols at link
 *   time), scripts use `mqtt.X()` directly rather than `import mqtt`.
 *
 * Design (from plan-berry-scripting.md §6.2):
 *
 *   mqtt.subscribe(filter, fn)
 *     Registers a {filter, fn} pair in the Berry-side global list
 *     `_be_mqtt_subs`. The broker's fanout loop calls
 *     berry_publish_topic_event() after every accepted PUBLISH; berry_task
 *     then calls dispatch_topic() which iterates the list and invokes any
 *     matching fn(topic, payload).
 *
 *   mqtt.unsubscribe(filter)
 *     Removes all entries whose filter matches the given string.
 *
 *   mqtt.publish(topic, payload [, qos=0 [, retain=false]])
 *     Posts to broker_publish_local() — the same tester/scheduler queue
 *     used by portal_ws.c and timers.c. Never opens a TCP loopback.
 *
 * Threading:
 *   All native functions run on berry_task. subscribe/unsubscribe mutate
 *   `_be_mqtt_subs` (a Berry list owned by the VM) — safe because the VM
 *   is single-threaded (berry_task). publish() calls broker_publish_local()
 *   which is thread-safe by design.
 *
 * Subscription list format:
 *   _be_mqtt_subs = [ [filter_str, fn], [filter_str, fn], ... ]
 *   berry_runtime.c::dispatch_topic() reads this directly.
 *   After every change we call berry_set_topic_subs_count() so broker_task
 *   can skip the event allocation fast-path when no listeners are armed.
 */

#include "berry_mod_mqtt.h"
#include "berry_runtime.h"
#include "mqtt_broker.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "berry.h"

static const char *TAG = "berry_mqtt";

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

/* Re-count entries in _be_mqtt_subs and update the runtime cache. Must be
 * called from berry_task after any mutation of the list. */
static void refresh_subs_count(bvm *vm)
{
    int top = be_top(vm);
    int count = 0;
    if (be_getglobal(vm, "_be_mqtt_subs") && be_islist(vm, -1)) {
        count = be_data_size(vm, -1);
    }
    be_pop(vm, be_top(vm) - top);
    berry_set_topic_subs_count(count);
}

/* ---------------------------------------------------------------------------
 * mqtt.subscribe(filter, fn)
 *
 * Berry signature: mqtt.subscribe(filter:string, fn:function) -> nil
 * -------------------------------------------------------------------------*/
static int m_subscribe(bvm *vm)
{
    int argc = be_top(vm);
    if (argc < 2 || !be_isstring(vm, 1) || !be_isfunction(vm, 2)) {
        be_raise(vm, "type_error",
                 "mqtt.subscribe expects (string filter, function fn)");
        be_return_nil(vm);
    }

    const char *filter = be_tostring(vm, 1);
    if (!filter || strlen(filter) == 0) {
        be_raise(vm, "value_error", "mqtt.subscribe: filter must not be empty");
        be_return_nil(vm);
    }

    int top = be_top(vm);

    /* Fetch (or create) the global subscription list. */
    if (!be_getglobal(vm, "_be_mqtt_subs") || !be_islist(vm, -1)) {
        be_pop(vm, be_top(vm) - top);
        be_newlist(vm);
        be_setglobal(vm, "_be_mqtt_subs");
        /* setglobal pops; push back so we can append. */
        be_getglobal(vm, "_be_mqtt_subs");
    }
    /* stack top: list */

    /* Build pair = [filter, fn] */
    be_newlist(vm);            /* pair */
    be_pushstring(vm, filter); /* pair[0] = filter */
    be_data_push(vm, -2);
    be_pop(vm, 1);
    be_pushvalue(vm, 2);       /* pair[1] = fn (original arg) */
    be_data_push(vm, -2);
    be_pop(vm, 1);
    /* stack: list, pair */

    /* Append pair to list. */
    be_data_push(vm, -2);
    be_pop(vm, 1); /* data_push copies; drop extra */
    /* stack: list */
    be_pop(vm, 1); /* list */

    refresh_subs_count(vm);
    ESP_LOGI(TAG, "subscribe: +\"%s\"", filter);

    be_return_nil(vm);
}

/* ---------------------------------------------------------------------------
 * mqtt.unsubscribe(filter)
 *
 * Removes all pairs whose filter == the given string.
 * Berry signature: mqtt.unsubscribe(filter:string) -> int  (# removed)
 * -------------------------------------------------------------------------*/
static int m_unsubscribe(bvm *vm)
{
    int argc = be_top(vm);
    if (argc < 1 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error",
                 "mqtt.unsubscribe expects (string filter)");
        be_return_nil(vm);
    }
    const char *target = be_tostring(vm, 1);
    size_t tlen = target ? strlen(target) : 0;

    int top = be_top(vm);
    if (!be_getglobal(vm, "_be_mqtt_subs") || !be_islist(vm, -1)) {
        be_pop(vm, be_top(vm) - top);
        be_pushint(vm, 0);
        be_return(vm);
    }
    /* stack top: list */
    int n = be_data_size(vm, -1);
    int removed = 0;
    /* Walk backwards so removal by index doesn't skip entries. */
    for (int i = n - 1; i >= 0; i--) {
        /* Get pair = list[i]. Stack: [list, i, pair] then remove i */
        be_pushint(vm, i);
        if (!be_getindex(vm, -2)) { be_pop(vm, 2); continue; }
        be_remove(vm, -2); /* remove key; stack: [list, pair] */

        /* Get filter = pair[0]. Stack: [list, pair, 0, filter] then remove 0 */
        be_pushint(vm, 0);
        be_getindex(vm, -2);
        be_remove(vm, -2); /* remove key; stack: [list, pair, filter] */

        if (be_isstring(vm, -1)) {
            const char *f = be_tostring(vm, -1);
            if (f && strlen(f) == tlen && memcmp(f, target, tlen) == 0) {
                be_pop(vm, 2); /* filter + pair; stack: [list] */
                be_pushint(vm, i);
                be_data_remove(vm, -2); /* remove list[i] */
                be_pop(vm, 1); /* key */
                removed++;
                continue;
            }
        }
        be_pop(vm, 2); /* filter + pair; stack: [list] */
    }
    be_pop(vm, 1); /* list */

    refresh_subs_count(vm);
    ESP_LOGI(TAG, "unsubscribe: \"%s\" removed %d entry(s)", target, removed);

    be_pushint(vm, removed);
    be_return(vm);
}

/* ---------------------------------------------------------------------------
 * mqtt.publish(topic, payload [, qos=0 [, retain=false]])
 *
 * Routes through broker_publish_local() — the same queue used by timers.c
 * and portal_ws.c. Never opens a TCP connection.
 * -------------------------------------------------------------------------*/
static int m_publish(bvm *vm)
{
    int argc = be_top(vm);
    if (argc < 2 || !be_isstring(vm, 1)) {
        be_raise(vm, "type_error",
                 "mqtt.publish expects (string topic, string payload [, int qos [, bool retain]])");
        be_return_nil(vm);
    }

    const char *topic   = be_tostring(vm, 1);
    size_t      tlen    = topic ? strlen(topic) : 0;

    const char *payload = "";
    size_t      plen    = 0;
    if (argc >= 2 && be_isstring(vm, 2)) {
        payload = be_tostring(vm, 2);
        plen    = payload ? strlen(payload) : 0;
    }

    uint8_t qos    = 0;
    bool    retain = false;
    if (argc >= 3 && be_isint(vm, 3)) {
        int q = (int)be_toint(vm, 3);
        qos = (q >= 1) ? 1 : 0;
    }
    if (argc >= 4 && be_isbool(vm, 4)) {
        retain = be_tobool(vm, 4);
    }

    if (tlen == 0) {
        be_raise(vm, "value_error", "mqtt.publish: topic must not be empty");
        be_return_nil(vm);
    }
    if (tlen > BROKER_TESTER_MAX_TOPIC_LEN) {
        be_raise(vm, "value_error", "mqtt.publish: topic too long");
        be_return_nil(vm);
    }
    if (plen > BROKER_TESTER_MAX_PAYLOAD_LEN) {
        be_raise(vm, "value_error", "mqtt.publish: payload too long");
        be_return_nil(vm);
    }

    bool ok = broker_publish_local(topic, tlen,
                                   (const uint8_t *)payload, plen,
                                   qos, retain);
    be_pushbool(vm, ok);
    be_return(vm);
}

/* ---------------------------------------------------------------------------
 * berry_mod_mqtt_register(vm)
 *
 * Called from berry_runtime.c::vm_construct() immediately after the VM is
 * constructed. Builds a module object with subscribe/unsubscribe/publish as
 * members and stores it as the global `mqtt`.
 *
 * Scripts call mqtt.subscribe("t/#", cb) directly (no `import` needed).
 * -------------------------------------------------------------------------*/
void berry_mod_mqtt_register(bvm *vm)
{
    if (!vm) return;
    int top = be_top(vm);

    /* Create a new module object and populate it. */
    be_newmodule(vm);          /* push module */

    be_pushntvfunction(vm, m_subscribe);
    be_setmember(vm, -2, "subscribe");
    be_pop(vm, 1);

    be_pushntvfunction(vm, m_unsubscribe);
    be_setmember(vm, -2, "unsubscribe");
    be_pop(vm, 1);

    be_pushntvfunction(vm, m_publish);
    be_setmember(vm, -2, "publish");
    be_pop(vm, 1);

    /* Store as global `mqtt`. setglobal does NOT pop the module. */
    be_setglobal(vm, "mqtt");

    be_pop(vm, be_top(vm) - top);

    const char *msg = "[P3] mqtt module registered (subscribe/unsubscribe/publish)\n";
    berry_port_stdout_write(msg, strlen(msg));
    ESP_LOGI(TAG, "mqtt module registered");
}
