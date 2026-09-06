/*
 * GEmu GPIO bridge
 *
 * Copyright (c) 2026 GEmu contributors
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#pragma once

#include "chardev/char-fe.h"
#include "hw/qdev-core.h"
#include "qemu/timer.h"

#define TYPE_GEMU_GPIO_BRIDGE "gemu-gpio-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(GEmuGPIOBridgeState, GEMU_GPIO_BRIDGE)

#define GEMU_GPIO_BRIDGE_PIN_COUNT 22
#define GEMU_GPIO_BRIDGE_MAX_LINE_SIZE (64 * 1024)
#define GEMU_GPIO_BRIDGE_MAX_EVENTS 4096

struct GEmuGPIOBridgeState {
    DeviceState parent_obj;

    CharBackend chardev;
    QEMUTimer timer;
    GQueue *events;
    GHashTable *sequences;
    qemu_irq input[GEMU_GPIO_BRIDGE_PIN_COUNT];

    uint8_t rx_buffer[GEMU_GPIO_BRIDGE_MAX_LINE_SIZE + 1];
    size_t rx_length;
    bool dropping_oversize_line;
    bool connected;
    bool hello_done;
    bool protocol_failed;

    bool output_level[GEMU_GPIO_BRIDGE_PIN_COUNT];
    bool output_enable[GEMU_GPIO_BRIDGE_PIN_COUNT];
    int8_t effective_drive[GEMU_GPIO_BRIDGE_PIN_COUNT];
};
