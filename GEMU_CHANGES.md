# GEmu 对 Espressif QEMU 的修改

本源码树以 Espressif QEMU tag `esp-develop-9.2.2-20260417`、commit `40edccac415693c5130f91c01d84176ae6008566` 为基础。GEmu contributors 于 2026-08-31 为 ESP32-C3 固件仿真原型作出以下修改：

- 为 ESP32-C3 GPIO0–21 实现裁剪的 direct digital GPIO 寄存器、输入、输出、output-enable 和中断语义；
- 通过 named qdev GPIO line 暴露 pin level、direction 和外部输入；
- 新增独立 `gemu-gpio-bridge` QOM device，以 chardev 承载 GEmu GPIO Bridge Protocol v1；
- 把 GPIO IRQ 接入既有 ESP32-C3 interrupt matrix，并把 GPIO 与 bridge line 接线；
- 未修改 TCG、RISC-V translator、QEMU main loop 或通用 QMP/QAPI schema。

修改过的既有文件和新增文件可通过本源码树识别。公开 fork 位于 `https://github.com/gzbigegg/qemu`，本次原型由不可变 tag `gemu-esp32c3-m1-prototype-0.0.1` 固定。本地转移验证包附带不含 ROM bytes 的工作树快照，但在构建期下载的三个 Meson 子项目源码归档补齐前，不把该快照称为完整 corresponding source，也不据此公开分发 binary。本文件不是对各源码文件 SPDX 标识的替代；具体许可证仍以 `LICENSE`、`COPYING`、`COPYING.LIB` 和各文件头为准。
