/*
 * ESP32-C3 GPIO emulation
 *
 * Copyright (c) 2023 Espressif Systems (Shanghai) Co. Ltd.
 * Modified by GEmu contributors on 2026-08-31 to implement direct digital GPIO.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/gpio/esp32c3_gpio.h"


static void esp32c3_gpio_update_lines(qemu_irq *lines, uint32_t old_value,
                                      uint32_t new_value)
{
    uint32_t changed = old_value ^ new_value;

    for (int pin = 0; pin < ESP32C3_GPIO_PIN_COUNT; pin++) {
        if (changed & BIT(pin)) {
            qemu_set_irq(lines[pin], (new_value & BIT(pin)) != 0);
        }
    }
}

static ESP32C3GPIOInterruptType esp32c3_gpio_interrupt_type(
    ESP32C3GPIOState *s, int pin)
{
    return (s->pin_config[pin] & ESP32C3_GPIO_PIN_INT_TYPE_MASK) >>
           ESP32C3_GPIO_PIN_INT_TYPE_SHIFT;
}

static bool esp32c3_gpio_interrupt_enabled(ESP32C3GPIOState *s, int pin)
{
    uint32_t interrupt_enable =
        (s->pin_config[pin] & ESP32C3_GPIO_PIN_INT_ENA_MASK) >>
        ESP32C3_GPIO_PIN_INT_ENA_SHIFT;

    return (interrupt_enable & ESP32C3_GPIO_PRO_CPU_INTR_ENA) != 0;
}

static uint32_t esp32c3_gpio_enabled_status(ESP32C3GPIOState *s)
{
    uint32_t enabled_status = 0;

    for (int pin = 0; pin < ESP32C3_GPIO_PIN_COUNT; pin++) {
        if (esp32c3_gpio_interrupt_enabled(s, pin)) {
            enabled_status |= s->status & BIT(pin);
        }
    }
    return enabled_status;
}

static void esp32c3_gpio_update_irq(ESP32C3GPIOState *s)
{
    bool asserted = esp32c3_gpio_enabled_status(s) != 0;

    if (asserted != s->irq_asserted) {
        s->irq_asserted = asserted;
        qemu_set_irq(s->parent.irq, asserted);
    }
}

static void esp32c3_gpio_reassert_level_interrupts(ESP32C3GPIOState *s)
{
    for (int pin = 0; pin < ESP32C3_GPIO_PIN_COUNT; pin++) {
        bool level = (s->input_level & BIT(pin)) != 0;

        switch (esp32c3_gpio_interrupt_type(s, pin)) {
        case ESP32C3_GPIO_INTR_LOW_LEVEL:
            if (!level) {
                s->status |= BIT(pin);
            }
            break;
        case ESP32C3_GPIO_INTR_HIGH_LEVEL:
            if (level) {
                s->status |= BIT(pin);
            }
            break;
        default:
            break;
        }
    }
}

static void esp32c3_gpio_set_input(void *opaque, int pin, int value)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);
    bool old_level;
    bool new_level;
    ESP32C3GPIOInterruptType interrupt_type;

    assert(pin >= 0 && pin < ESP32C3_GPIO_PIN_COUNT);
    if (value < 0) {
        return;
    }

    old_level = (s->input_level & BIT(pin)) != 0;
    new_level = value != 0;
    s->input_level = deposit32(s->input_level, pin, 1, new_level);
    interrupt_type = esp32c3_gpio_interrupt_type(s, pin);

    if ((!old_level && new_level &&
         (interrupt_type == ESP32C3_GPIO_INTR_POSEDGE ||
          interrupt_type == ESP32C3_GPIO_INTR_ANYEDGE)) ||
        (old_level && !new_level &&
         (interrupt_type == ESP32C3_GPIO_INTR_NEGEDGE ||
          interrupt_type == ESP32C3_GPIO_INTR_ANYEDGE)) ||
        (!new_level && interrupt_type == ESP32C3_GPIO_INTR_LOW_LEVEL) ||
        (new_level && interrupt_type == ESP32C3_GPIO_INTR_HIGH_LEVEL)) {
        s->status |= BIT(pin);
    }
    esp32c3_gpio_update_irq(s);
}

static uint64_t esp32c3_gpio_read(void *opaque, hwaddr addr,
                                  unsigned int size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);

    switch (addr) {
    case A_GPIO_OUT:
        return s->output_level;
    case A_GPIO_ENABLE:
        return s->output_enable;
    case A_GPIO_IN:
        return s->input_level;
    case A_GPIO_STATUS:
        return s->status;
    case A_GPIO_PCPU_INT:
        return esp32c3_gpio_enabled_status(s);
    case A_GPIO_STRAP:
        return s->parent.strap_mode;
    default:
        if (addr >= ESP32C3_GPIO_PIN0_OFFSET &&
            addr < ESP32C3_GPIO_PIN0_OFFSET +
                   ESP32C3_GPIO_PIN_COUNT * ESP32C3_GPIO_PIN_STRIDE &&
            (addr - ESP32C3_GPIO_PIN0_OFFSET) % ESP32C3_GPIO_PIN_STRIDE == 0) {
            int pin = (addr - ESP32C3_GPIO_PIN0_OFFSET) /
                      ESP32C3_GPIO_PIN_STRIDE;
            return s->pin_config[pin];
        }
        return 0;
    }
}

static void esp32c3_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(opaque);
    uint32_t masked_value = value & ESP32C3_GPIO_VALID_MASK;
    uint32_t old_value;

    switch (addr) {
    case A_GPIO_OUT:
        old_value = s->output_level;
        s->output_level = masked_value;
        esp32c3_gpio_update_lines(s->level, old_value, s->output_level);
        break;
    case A_GPIO_OUT_W1TS:
        old_value = s->output_level;
        s->output_level |= masked_value;
        esp32c3_gpio_update_lines(s->level, old_value, s->output_level);
        break;
    case A_GPIO_OUT_W1TC:
        old_value = s->output_level;
        s->output_level &= ~masked_value;
        esp32c3_gpio_update_lines(s->level, old_value, s->output_level);
        break;
    case A_GPIO_ENABLE:
        old_value = s->output_enable;
        s->output_enable = masked_value;
        esp32c3_gpio_update_lines(s->output_enable_lines, old_value,
                                  s->output_enable);
        break;
    case A_GPIO_ENABLE_W1TS:
        old_value = s->output_enable;
        s->output_enable |= masked_value;
        esp32c3_gpio_update_lines(s->output_enable_lines, old_value,
                                  s->output_enable);
        break;
    case A_GPIO_ENABLE_W1TC:
        old_value = s->output_enable;
        s->output_enable &= ~masked_value;
        esp32c3_gpio_update_lines(s->output_enable_lines, old_value,
                                  s->output_enable);
        break;
    case A_GPIO_STATUS_W1TS:
        s->status |= masked_value;
        esp32c3_gpio_update_irq(s);
        break;
    case A_GPIO_STATUS_W1TC:
        s->status &= ~masked_value;
        esp32c3_gpio_reassert_level_interrupts(s);
        esp32c3_gpio_update_irq(s);
        break;
    default:
        if (addr >= ESP32C3_GPIO_PIN0_OFFSET &&
            addr < ESP32C3_GPIO_PIN0_OFFSET +
                   ESP32C3_GPIO_PIN_COUNT * ESP32C3_GPIO_PIN_STRIDE &&
            (addr - ESP32C3_GPIO_PIN0_OFFSET) % ESP32C3_GPIO_PIN_STRIDE == 0) {
            int pin = (addr - ESP32C3_GPIO_PIN0_OFFSET) /
                      ESP32C3_GPIO_PIN_STRIDE;
            s->pin_config[pin] = value;
            esp32c3_gpio_reassert_level_interrupts(s);
            esp32c3_gpio_update_irq(s);
        }
        break;
    }
}

static const MemoryRegionOps esp32c3_gpio_ops = {
    .read = esp32c3_gpio_read,
    .write = esp32c3_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void esp32c3_gpio_reset_hold(Object *obj, ResetType type)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);
    uint32_t old_output_level = s->output_level;
    uint32_t old_output_enable = s->output_enable;

    s->output_level = 0;
    s->output_enable = 0;
    s->input_level = 0;
    s->status = 0;
    memset(s->pin_config, 0, sizeof(s->pin_config));
    esp32c3_gpio_update_lines(s->level, old_output_level, 0);
    esp32c3_gpio_update_lines(s->output_enable_lines, old_output_enable, 0);
    if (s->irq_asserted) {
        s->irq_asserted = false;
        qemu_set_irq(s->parent.irq, 0);
    }
}

static void esp32c3_gpio_init(Object *obj)
{
    ESP32C3GPIOState *s = ESP32C3_GPIO(obj);

    /* Set the default value for the property */
    object_property_set_int(obj, "strap_mode", ESP32C3_STRAP_MODE_FLASH_BOOT, &error_fatal);
    qdev_init_gpio_out_named(DEVICE(obj), s->level, "level",
                             ESP32C3_GPIO_PIN_COUNT);
    qdev_init_gpio_out_named(DEVICE(obj), s->output_enable_lines,
                             "output-enable", ESP32C3_GPIO_PIN_COUNT);
    qdev_init_gpio_in_named(DEVICE(obj), esp32c3_gpio_set_input, "input",
                            ESP32C3_GPIO_PIN_COUNT);
}

/* If we need to override any function from the parent (reset, realize, ...), it shall be done
 * in this class_init function */
static void esp32c3_gpio_class_init(ObjectClass *klass, void *data)
{
    Esp32GpioClass *gc = ESP32_GPIO_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    gc->ops = &esp32c3_gpio_ops;
    rc->phases.hold = esp32c3_gpio_reset_hold;
}

static const TypeInfo esp32c3_gpio_info = {
    .name = TYPE_ESP32C3_GPIO,
    .parent = TYPE_ESP32_GPIO,
    .instance_size = sizeof(ESP32C3GPIOState),
    .instance_init = esp32c3_gpio_init,
    .class_init = esp32c3_gpio_class_init,
    .class_size = sizeof(ESP32C3GPIOClass),
};

static void esp32c3_gpio_register_types(void)
{
    type_register_static(&esp32c3_gpio_info);
}

type_init(esp32c3_gpio_register_types)
