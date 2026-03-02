# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

---

## [Unreleased] — ESPHome 2026.6 Compatibility (deprecation follow-up)

### Changed

#### `components/truma_inetbox/__init__.py`
- `CORE.using_esp_idf` → `CORE.is_esp32 and not CORE.using_arduino`
  Deprecated since ESPHome 2026.1 (behavior change in 2026.6). The condition targets
  ESP-IDF-only builds where the `ARDUINO_SERIAL_EVENT_TASK_*` macros are not provided
  by the framework.

#### `components/uart/__init__.py`
- `CORE.using_esp_idf` → `not CORE.using_arduino`
  Same deprecation fix in the UART type selector (`_uart_declare_type`).

#### `components/truma_inetbox/LinBusListener_esp_idf.cpp`
- Added `#ifndef` fallback defines for `ARDUINO_SERIAL_EVENT_TASK_STACK_SIZE` (4096)
  and `ARDUINO_SERIAL_EVENT_TASK_RUNNING_CORE` (0) so the file compiles even without
  the build flags as a safety net.

#### `components/truma_inetbox/climate/TrumaWaterClimate.cpp`
#### `components/truma_inetbox/climate/TrumaRoomClimate.cpp`
- `traits.set_supports_current_temperature(true)`
  → `traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE)`
  `set_supports_current_temperature` is deprecated in ESPHome 2025+.

---

## [Unreleased] — ESPHome 2025.8+ / 2026.2.x Compatibility

### Summary

This release restores compatibility with ESPHome 2025.8 through 2026.2.x.
The primary driver was the removal of `get_uart_event_queue()` from the upstream
`IDFUARTComponent` in ESPHome 2025.8, which broke LIN-bus BREAK detection on
ESP-IDF builds. Additional breaking changes in ESP-IDF 5.x (ESP32 toolchain) and
ESPHome 2026.x API changes were also resolved.

Tested against:
- ESPHome **2025.9.3** — Arduino + ESP-IDF ✅
- ESPHome **2026.2.2** — Arduino + ESP-IDF ✅

---

### Changed — `components/uart/`

#### `uart_component.h`
- `virtual int available()` → `virtual size_t available()` to match ESPHome 2025.8+ signature
- Added default (no-op) implementations for new virtual methods introduced in ESPHome 2025.8:
  `set_rx_full_threshold()`, `set_rx_timeout()`, `load_settings(bool)`, `load_settings()`

#### `uart_component.cpp`
- Updated `check_read_timeout_()` to use `size_t` comparisons (no unnecessary `int` casts)

#### `uart_component_esp_idf.h` _(critical)_
- Preprocessor guard changed: `USE_ESP_IDF` → `USE_ESP32_FRAMEWORK_ESP_IDF`
- Removed `SemaphoreHandle_t lock_` member (mutex removed from upstream in 2025.8)
- `int available()` → `size_t available()`
- Added `get_hw_serial_number()` directly to `IDFUARTComponent` base class
- Added declarations for `load_settings()`, `set_rx_full_threshold()`, `set_rx_timeout()`
- `uart_event_queue_` kept **unconditionally** (not guarded by `USE_UART_WAKE_LOOP_ON_RX`),
  because the LIN-bus BREAK detection task requires it at all times

#### `uart_component_esp_idf.cpp` _(critical)_
- Preprocessor guard changed: `USE_ESP_IDF` → `USE_ESP32_FRAMEWORK_ESP_IDF`
- `UART_SCLK_APB` → `UART_SCLK_DEFAULT` (ESP-IDF 5.x API change)
- `portTICK_RATE_MS` → `pdMS_TO_TICKS(20)` (removed from ESP-IDF 5.x)
- All `lock_` mutex take/give calls removed (~12 call-sites)
- `static uint8_t next_uart_num` → `static uart_port_t next_uart_num = UART_NUM_0`
  (ESP-IDF 5.x: `uart_port_t` is a scoped enum, no implicit `uint8_t` conversion)
- Postfix `++` on `uart_port_t` replaced with explicit cast:
  `next_uart_num = (uart_port_t)(next_uart_num + 1)`
- `int available()` → `size_t available()`
- Added implementations for `load_settings()`, `set_rx_full_threshold()`, `set_rx_timeout()`

#### `truma_uart_component_esp_idf.h`
- Preprocessor guard changed: `USE_ESP_IDF` → `USE_ESP32_FRAMEWORK_ESP_IDF`
- Removed `get_hw_serial_number()` (now provided by `IDFUARTComponent` base class)
- Retains `get_uart_event_queue()` exposing `&uart_event_queue_`

#### `uart_component_esp32_arduino.h` / `.cpp`
- `int available()` → `size_t available()`
- `check_logger_conflict()`: `logger::global_logger->get_hw_serial()` guarded with
  `#if defined(USE_LOGGER) && !defined(USE_ESP32)` — ESPHome 2026.1 removed
  `get_hw_serial()` from `Logger` for ESP32 (Arduino on ESP32 now builds on IDF)

#### `uart_component_rp2040.h` / `.cpp`
- `int available()` → `size_t available()`

#### `uart_component_esp8266.h` / `.cpp`
- `ESP8266UartComponent::available()`: `int` → `size_t`

---

### Changed — `components/truma_inetbox/`

#### POSIX integer type replacements (all 30 affected files)
- `u_int8_t` → `uint8_t`
- `u_int16_t` → `uint16_t`
- `u_int32_t` → `uint32_t`

These POSIX-style types (`u_int*_t`) are defined implicitly by glibc / BSD libc headers
that the Arduino toolchain includes automatically. The ESP-IDF 5.x GCC toolchain does
**not** make them available, causing 294 compile errors across 30 files.

Affected files include:
`LinBusProtocol.h`, `LinBusProtocol.cpp`, `LinBusListener.h`, `LinBusListener.cpp`,
`TrumaiNetBoxApp.h`, `TrumaiNetBoxApp.cpp`, `TrumaiNetBoxAppHeater.h/cpp`,
`TrumaiNetBoxAppAirconManual.h/cpp`, `TrumaiNetBoxAppAirconAuto.h/cpp`,
`TrumaiNetBoxAppClock.h/cpp`, `TrumaiNetBoxAppTimer.h/cpp`,
`TrumaStructs.h`, `TrumaEnums.h`, `TrumaStatusFrameBuilder.h`,
`TrumaStausFrameResponseStorage.h`, `helpers.h`, `helpers.cpp`,
`automation.h`, `time/TrumaTime.h`, and sensor/number/select/climate sub-components.

#### `LinBusListener_esp_idf.cpp`
- `#define QUEUE_WAIT_BLOCKING (portTickType) portMAX_DELAY`
  → `(TickType_t) portMAX_DELAY`
  (`portTickType` was renamed to `TickType_t` in FreeRTOS 10 / ESP-IDF 5.x)
- `uart_intr_config(uart_num, &uart_intr)` → `uart_intr_config((uart_port_t) uart_num, &uart_intr)`
  (ESP-IDF 5.x: `uart_intr_config` requires `uart_port_t`, no implicit `uint8_t` conversion)

#### `LinBusListener_esp32_arduino.cpp`
- `#define QUEUE_WAIT_BLOCKING (portTickType) portMAX_DELAY`
  → `(TickType_t) portMAX_DELAY`
  (same FreeRTOS rename, also affects Arduino on ESP32 which builds on ESP-IDF 5.x)

---

### Added

- `test_compile.yaml` — minimal test configuration for ESP32 Arduino framework builds
- `test_compile_idf.yaml` — minimal test configuration for ESP32 ESP-IDF framework builds

---

### Notes

- ESPHome **2026.1.x does not exist** on PyPI — version numbering jumps from 2025.10.x
  directly to 2026.2.x.
- ESPHome 2026.1 deprecated `CORE.using_esp_idf` (warning only; behavior changes in 2026.6).
  ESP32 Arduino now officially builds on top of ESP-IDF, so IDF features are available
  in both frameworks. The `uart_component_esp32_arduino` component continues to work
  as a custom override for this project.
- Installing ESPHome 2026.2.x in a Python venv additionally requires the
  `fatfs-ng` package (`pip install fatfs-ng`) as a transitive PlatformIO dependency.
