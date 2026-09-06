/*
 * Modified by GEmu contributors on 2026-08-31 to model direct digital GPIO.
 * This work remains under the license terms documented by the QEMU source tree.
 */

#pragma once

#include "hw/sysbus.h"
#include "hw/hw.h"
#include "hw/registerfields.h"
#include "esp32_gpio.h"

#define TYPE_ESP32C3_GPIO "esp32c3.gpio"
#define ESP32C3_GPIO(obj)           OBJECT_CHECK(ESP32C3GPIOState, (obj), TYPE_ESP32C3_GPIO)
#define ESP32C3_GPIO_GET_CLASS(obj) OBJECT_GET_CLASS(ESP32C3GPIOClass, obj, TYPE_ESP32C3_GPIO)
#define ESP32C3_GPIO_CLASS(klass)   OBJECT_CLASS_CHECK(ESP32C3GPIOClass, klass, TYPE_ESP32C3_GPIO)

/* Bootstrap options for ESP32-C3 (4-bit) */
#define ESP32C3_STRAP_MODE_FLASH_BOOT 0x8   /* SPI Boot */
#define ESP32C3_STRAP_MODE_UART_BOOT  0x2   /* Diagnostic Mode0+UART0 download Mode */
#define ESP32C3_STRAP_MODE_USB_BOOT   0x0   /* Diagnostic Mode1+USB download Mode */

#define ESP32C3_GPIO_PIN_COUNT 22
#define ESP32C3_GPIO_VALID_MASK ((1U << ESP32C3_GPIO_PIN_COUNT) - 1)

REG32(GPIO_OUT, 0x0004)
REG32(GPIO_OUT_W1TS, 0x0008)
REG32(GPIO_OUT_W1TC, 0x000c)
REG32(GPIO_ENABLE, 0x0020)
REG32(GPIO_ENABLE_W1TS, 0x0024)
REG32(GPIO_ENABLE_W1TC, 0x0028)
REG32(GPIO_IN, 0x003c)
REG32(GPIO_STATUS, 0x0044)
REG32(GPIO_STATUS_W1TS, 0x0048)
REG32(GPIO_STATUS_W1TC, 0x004c)
REG32(GPIO_PCPU_INT, 0x005c)

#define ESP32C3_GPIO_PIN0_OFFSET 0x0074
#define ESP32C3_GPIO_PIN_STRIDE  0x0004
#define ESP32C3_GPIO_PIN_INT_TYPE_SHIFT 7
#define ESP32C3_GPIO_PIN_INT_TYPE_MASK  (0x7U << ESP32C3_GPIO_PIN_INT_TYPE_SHIFT)
#define ESP32C3_GPIO_PIN_INT_ENA_SHIFT  13
#define ESP32C3_GPIO_PIN_INT_ENA_MASK   (0x1fU << ESP32C3_GPIO_PIN_INT_ENA_SHIFT)
#define ESP32C3_GPIO_PRO_CPU_INTR_ENA   BIT(0)

typedef enum ESP32C3GPIOInterruptType {
    ESP32C3_GPIO_INTR_DISABLE = 0,
    ESP32C3_GPIO_INTR_POSEDGE = 1,
    ESP32C3_GPIO_INTR_NEGEDGE = 2,
    ESP32C3_GPIO_INTR_ANYEDGE = 3,
    ESP32C3_GPIO_INTR_LOW_LEVEL = 4,
    ESP32C3_GPIO_INTR_HIGH_LEVEL = 5,
} ESP32C3GPIOInterruptType;

typedef struct ESP32C3State {
    Esp32GpioState parent;

    uint32_t output_level;
    uint32_t output_enable;
    uint32_t input_level;
    uint32_t status;
    uint32_t pin_config[ESP32C3_GPIO_PIN_COUNT];
    bool irq_asserted;
    qemu_irq level[ESP32C3_GPIO_PIN_COUNT];
    qemu_irq output_enable_lines[ESP32C3_GPIO_PIN_COUNT];
} ESP32C3GPIOState;

typedef struct ESP32C3GPIOClass {
    Esp32GpioClass parent;
} ESP32C3GPIOClass;
