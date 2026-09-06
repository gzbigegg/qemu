/*
 * GEmu GPIO bridge
 *
 * Copyright (c) 2026 GEmu contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qnum.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/misc/gemu_gpio_bridge.h"

#define GEMU_GPIO_BRIDGE_COMMIT "40edccac415693c5130f91c01d84176ae6008566"

typedef struct GEmuGPIOEvent {
    uint64_t at_ns;
    uint64_t seq;
    uint8_t pin;
    bool level;
} GEmuGPIOEvent;

static void gemu_gpio_bridge_send(GEmuGPIOBridgeState *s, const char *message)
{
    int length;
    int written;

    if (!s->connected || s->protocol_failed) {
        return;
    }

    length = strlen(message);
    written = qemu_chr_fe_write(&s->chardev, (const uint8_t *)message,
                                length);
    if (written != length) {
        s->protocol_failed = true;
        error_report("GEmu GPIO bridge output failed: wrote %d of %d bytes",
                     written, length);
    }
}

static void gemu_gpio_bridge_send_error(GEmuGPIOBridgeState *s, bool has_seq,
                                        uint64_t seq, const char *code,
                                        const char *message)
{
    g_autofree char *response = NULL;

    if (has_seq) {
        response = g_strdup_printf(
            "{\"v\":1,\"seq\":%" PRIu64
            ",\"type\":\"error\",\"code\":\"%s\",\"message\":\"%s\"}\n",
            seq, code, message);
    } else {
        response = g_strdup_printf(
            "{\"v\":1,\"type\":\"error\",\"code\":\"%s\","
            "\"message\":\"%s\"}\n",
            code, message);
    }
    gemu_gpio_bridge_send(s, response);
}

static bool gemu_gpio_bridge_get_uint(const QDict *message, const char *name,
                                      uint64_t *value)
{
    QNum *number = qobject_to(QNum, qdict_get(message, name));

    return number && qnum_get_try_uint(number, value);
}

static gint gemu_gpio_bridge_compare_events(gconstpointer left,
                                            gconstpointer right,
                                            gpointer opaque)
{
    const GEmuGPIOEvent *a = left;
    const GEmuGPIOEvent *b = right;

    if (a->at_ns != b->at_ns) {
        return a->at_ns < b->at_ns ? -1 : 1;
    }
    if (a->seq != b->seq) {
        return a->seq < b->seq ? -1 : 1;
    }
    return 0;
}

static void gemu_gpio_bridge_arm_timer(GEmuGPIOBridgeState *s)
{
    GEmuGPIOEvent *next = g_queue_peek_head(s->events);

    if (next) {
        timer_mod_ns(&s->timer, next->at_ns);
    } else {
        timer_del(&s->timer);
    }
}

static void gemu_gpio_bridge_timer(void *opaque)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(opaque);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    GEmuGPIOEvent *event;

    while ((event = g_queue_peek_head(s->events)) && event->at_ns <= now) {
        g_autofree char *message = NULL;

        event = g_queue_pop_head(s->events);
        qemu_set_irq(s->input[event->pin], event->level);
        message = g_strdup_printf(
            "{\"v\":1,\"seq\":%" PRIu64
            ",\"type\":\"gpio.applied\",\"at_ns\":\"%" PRIu64
            "\",\"pin\":%u,\"level\":%u}\n",
            event->seq, event->at_ns, event->pin, event->level);
        gemu_gpio_bridge_send(s, message);
        g_free(event);
    }
    gemu_gpio_bridge_arm_timer(s);
}

static void gemu_gpio_bridge_handle_hello(GEmuGPIOBridgeState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    g_autofree char *response = g_strdup_printf(
        "{\"v\":1,\"type\":\"hello\",\"engine\":\"espressif-qemu\","
        "\"engine_commit\":\"" GEMU_GPIO_BRIDGE_COMMIT "\","
        "\"clock\":\"virtual-ns\",\"now_ns\":\"%" PRId64 "\",\"pins\":22,"
        "\"capabilities\":[\"gpio.schedule\",\"gpio.applied\",\"pin.drive\","
        "\"pin.direction\"]}\n", now);

    s->hello_done = true;
    gemu_gpio_bridge_send(s, response);
}

static void gemu_gpio_bridge_handle_schedule(GEmuGPIOBridgeState *s,
                                             const QDict *message)
{
    const char *at_ns_string = qdict_get_try_str(message, "at_ns");
    uint64_t seq;
    uint64_t pin;
    uint64_t level;
    uint64_t at_ns;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    GEmuGPIOEvent *event;
    uint64_t *sequence_key;
    g_autofree char *response = NULL;

    if (!gemu_gpio_bridge_get_uint(message, "seq", &seq)) {
        gemu_gpio_bridge_send_error(s, false, 0, "invalid_message",
                                    "seq must be an unsigned integer");
        return;
    }
    if (!at_ns_string || qemu_strtou64(at_ns_string, NULL, 10, &at_ns) < 0 ||
        at_ns > INT64_MAX) {
        gemu_gpio_bridge_send_error(s, true, seq, "invalid_message",
                                    "at_ns must be a decimal string");
        return;
    }
    if (!gemu_gpio_bridge_get_uint(message, "pin", &pin) ||
        pin >= GEMU_GPIO_BRIDGE_PIN_COUNT ||
        !gemu_gpio_bridge_get_uint(message, "level", &level) || level > 1) {
        gemu_gpio_bridge_send_error(s, true, seq, "invalid_message",
                                    "pin or level is out of range");
        return;
    }
    if (at_ns < now) {
        gemu_gpio_bridge_send_error(s, true, seq, "past_virtual_time",
                                    "at_ns is earlier than current virtual time");
        return;
    }
    if (g_hash_table_contains(s->sequences, &seq)) {
        gemu_gpio_bridge_send_error(s, true, seq, "duplicate_seq",
                                    "seq has already been used");
        return;
    }
    if (g_queue_get_length(s->events) >= GEMU_GPIO_BRIDGE_MAX_EVENTS) {
        gemu_gpio_bridge_send_error(s, true, seq, "queue_overflow",
                                    "scheduled event queue is full");
        return;
    }

    event = g_new0(GEmuGPIOEvent, 1);
    event->at_ns = at_ns;
    event->seq = seq;
    event->pin = pin;
    event->level = level;
    sequence_key = g_new(uint64_t, 1);
    *sequence_key = seq;
    g_hash_table_add(s->sequences, sequence_key);
    g_queue_insert_sorted(s->events, event, gemu_gpio_bridge_compare_events,
                          NULL);
    gemu_gpio_bridge_arm_timer(s);

    response = g_strdup_printf(
        "{\"v\":1,\"seq\":%" PRIu64
        ",\"type\":\"ack\",\"status\":\"scheduled\","
        "\"at_ns\":\"%" PRIu64 "\"}\n",
        seq, at_ns);
    gemu_gpio_bridge_send(s, response);
}

static void gemu_gpio_bridge_process_line(GEmuGPIOBridgeState *s,
                                          const char *line)
{
    Error *local_error = NULL;
    QObject *object = qobject_from_json(line, &local_error);
    QDict *message;
    const char *type;
    uint64_t version;

    if (!object) {
        error_free(local_error);
        gemu_gpio_bridge_send_error(s, false, 0, "bad_json",
                                    "line is not valid JSON");
        return;
    }
    message = qobject_to(QDict, object);
    if (!message || !gemu_gpio_bridge_get_uint(message, "v", &version) ||
        version != 1) {
        gemu_gpio_bridge_send_error(s, false, 0, "unsupported_version",
                                    "protocol major version must be 1");
        qobject_unref(object);
        return;
    }

    type = qdict_get_try_str(message, "type");
    if (type && !strcmp(type, "hello")) {
        gemu_gpio_bridge_handle_hello(s);
    } else if (!s->hello_done) {
        gemu_gpio_bridge_send_error(s, false, 0, "handshake_required",
                                    "hello must be sent first");
    } else if (type && !strcmp(type, "gpio.schedule")) {
        gemu_gpio_bridge_handle_schedule(s, message);
    } else {
        gemu_gpio_bridge_send_error(s, false, 0, "unsupported_type",
                                    "message type is not supported");
    }
    qobject_unref(object);
}

static int gemu_gpio_bridge_can_receive(void *opaque)
{
    return 4096;
}

static void gemu_gpio_bridge_receive(void *opaque, const uint8_t *buffer,
                                     int size)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(opaque);

    for (int index = 0; index < size; index++) {
        uint8_t value = buffer[index];

        if (s->dropping_oversize_line) {
            if (value == '\n') {
                s->dropping_oversize_line = false;
            }
            continue;
        }
        if (value == '\n') {
            if (s->rx_length && s->rx_buffer[s->rx_length - 1] == '\r') {
                s->rx_length--;
            }
            s->rx_buffer[s->rx_length] = 0;
            if (s->rx_length) {
                gemu_gpio_bridge_process_line(s, (char *)s->rx_buffer);
            }
            s->rx_length = 0;
            continue;
        }
        if (s->rx_length >= GEMU_GPIO_BRIDGE_MAX_LINE_SIZE) {
            s->rx_length = 0;
            s->dropping_oversize_line = true;
            gemu_gpio_bridge_send_error(s, false, 0, "frame_too_large",
                                        "NDJSON line exceeds 64 KiB");
            continue;
        }
        s->rx_buffer[s->rx_length++] = value;
    }
}

static void gemu_gpio_bridge_chardev_event(void *opaque, QEMUChrEvent event)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(opaque);

    switch (event) {
    case CHR_EVENT_OPENED:
        s->connected = true;
        s->hello_done = false;
        s->protocol_failed = false;
        s->rx_length = 0;
        s->dropping_oversize_line = false;
        break;
    case CHR_EVENT_CLOSED:
        if (s->hello_done || !g_queue_is_empty(s->events)) {
            error_report("GEmu GPIO bridge disconnected during a session");
        }
        s->connected = false;
        s->hello_done = false;
        break;
    case CHR_EVENT_BREAK:
    case CHR_EVENT_MUX_IN:
    case CHR_EVENT_MUX_OUT:
        break;
    }
}

static void gemu_gpio_bridge_update_drive(GEmuGPIOBridgeState *s, int pin)
{
    int8_t drive = s->output_enable[pin] ? s->output_level[pin] : -1;
    int64_t now;
    g_autofree char *message = NULL;

    if (drive == s->effective_drive[pin]) {
        return;
    }
    s->effective_drive[pin] = drive;
    if (!s->hello_done) {
        return;
    }

    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (drive < 0) {
        message = g_strdup_printf(
            "{\"v\":1,\"type\":\"pin.drive\",\"at_ns\":\"%" PRId64
            "\",\"pin\":%d,\"level\":null}\n",
            now, pin);
    } else {
        message = g_strdup_printf(
            "{\"v\":1,\"type\":\"pin.drive\",\"at_ns\":\"%" PRId64
            "\",\"pin\":%d,\"level\":%d}\n",
            now, pin, drive);
    }
    gemu_gpio_bridge_send(s, message);
}

static void gemu_gpio_bridge_set_level(void *opaque, int pin, int value)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(opaque);

    assert(pin >= 0 && pin < GEMU_GPIO_BRIDGE_PIN_COUNT);
    s->output_level[pin] = value != 0;
    gemu_gpio_bridge_update_drive(s, pin);
}

static void gemu_gpio_bridge_set_output_enable(void *opaque, int pin,
                                               int value)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(opaque);

    assert(pin >= 0 && pin < GEMU_GPIO_BRIDGE_PIN_COUNT);
    s->output_enable[pin] = value != 0;
    gemu_gpio_bridge_update_drive(s, pin);
}

static void gemu_gpio_bridge_realize(DeviceState *device, Error **errp)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(device);

    if (qemu_chr_fe_backend_connected(&s->chardev)) {
        qemu_chr_fe_set_handlers(&s->chardev,
                                 gemu_gpio_bridge_can_receive,
                                 gemu_gpio_bridge_receive,
                                 gemu_gpio_bridge_chardev_event,
                                 NULL, s, NULL, true);
    }
}

static void gemu_gpio_bridge_unrealize(DeviceState *device)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(device);

    qemu_chr_fe_set_handlers(&s->chardev, NULL, NULL, NULL, NULL, NULL, NULL,
                             false);
    timer_del(&s->timer);
}

static void gemu_gpio_bridge_init(Object *object)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(object);

    s->events = g_queue_new();
    s->sequences = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                         NULL);
    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL, gemu_gpio_bridge_timer, s);
    memset(s->effective_drive, -1, sizeof(s->effective_drive));
    qdev_init_gpio_in_named(DEVICE(object), gemu_gpio_bridge_set_level,
                            "level", GEMU_GPIO_BRIDGE_PIN_COUNT);
    qdev_init_gpio_in_named(DEVICE(object),
                            gemu_gpio_bridge_set_output_enable,
                            "output-enable", GEMU_GPIO_BRIDGE_PIN_COUNT);
    qdev_init_gpio_out_named(DEVICE(object), s->input, "input",
                             GEMU_GPIO_BRIDGE_PIN_COUNT);
}

static void gemu_gpio_bridge_finalize(Object *object)
{
    GEmuGPIOBridgeState *s = GEMU_GPIO_BRIDGE(object);

    g_queue_free_full(s->events, g_free);
    g_hash_table_destroy(s->sequences);
}

static Property gemu_gpio_bridge_properties[] = {
    DEFINE_PROP_CHR("chardev", GEmuGPIOBridgeState, chardev),
    DEFINE_PROP_END_OF_LIST(),
};

static void gemu_gpio_bridge_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = gemu_gpio_bridge_realize;
    dc->unrealize = gemu_gpio_bridge_unrealize;
    dc->desc = "GEmu versioned GPIO bridge";
    device_class_set_props(dc, gemu_gpio_bridge_properties);
}

static const TypeInfo gemu_gpio_bridge_info = {
    .name = TYPE_GEMU_GPIO_BRIDGE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GEmuGPIOBridgeState),
    .instance_init = gemu_gpio_bridge_init,
    .instance_finalize = gemu_gpio_bridge_finalize,
    .class_init = gemu_gpio_bridge_class_init,
};

static void gemu_gpio_bridge_register_types(void)
{
    type_register_static(&gemu_gpio_bridge_info);
}

type_init(gemu_gpio_bridge_register_types)
