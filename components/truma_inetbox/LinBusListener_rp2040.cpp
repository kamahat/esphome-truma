#ifdef USE_RP2040
// =============================================================================
// LinBusListener_rp2040.cpp — RP2040 backend for the LIN bus listener
// =============================================================================
//
// Architecture: two dedicated FreeRTOS tasks (mirrors the ESP32 design)
// -----------------------------------------------------------------------
//  lin_uart_task_  — high priority, pinned to Core TRUMA_LIN_TASK_CORE (default: 1)
//      Polls hardware UART, detects BREAK, calls answer_lin_order_() for real-time
//      LIN responses, enqueues complete frames in lin_msg_queue_.
//
//  lin_event_task_ — low priority, same core
//      Blocks on lin_msg_queue_ and dispatches frames to lin_message_recieved_().
//
// WHY NOT loop1()
// ---------------
// loop1() is a global weak symbol consumed by the arduino-pico runtime.
// Any other component that also defines loop1() (sensors using Core 1 for
// SPI/DMA, BMI160, etc.) causes a linker collision or silent override.
// Only one definition wins — the LIN bus or the other sensor, never both.
// Dedicated FreeRTOS tasks with explicit core affinity coexist safely with
// all other components, just as the ESP32 implementation already does.
//
// Defensive programming
// ---------------------
// Uses TRUMA_GUARD_* macros from LinBusGuards.h instead of try/catch.
// ESPHome/arduino-pico build with -fno-exceptions: try/catch is dead code.
// =============================================================================

#include "LinBusListener.h"
#include "esphome/core/log.h"
#ifdef CUSTOM_ESPHOME_UART
#include "esphome/components/uart/truma_uart_component_rp2040.h"
#define ESPHOME_UART uart::truma_RP2040UartComponent
#else
#define ESPHOME_UART uart::RP2040UartComponent
#endif  // CUSTOM_ESPHOME_UART
#include "esphome/components/uart/uart_component_rp2040.h"
#include <SerialUART.h>

// arduino-pico >= 5.x requires __FREERTOS defined before any FreeRTOS header.
#ifndef __FREERTOS
#define __FREERTOS 1
#endif
#include <task.h>

namespace esphome {
namespace truma_inetbox {

static const char *const TAG = "truma_inetbox.LinBusListener";

#define QUEUE_WAIT_DONT_BLOCK ((TickType_t) 0)
#define QUEUE_WAIT_BLOCKING   ((TickType_t) portMAX_DELAY)

// Module-level pointer so vApplicationStackOverflowHook can call on_fatal_error_().
static LinBusListener *g_lin_listener_for_stack_hook = nullptr;

// ---------------------------------------------------------------------------
// Helper: pin a task to a specific core (SMP kernels only).
// Falls back silently on single-core builds.
// ---------------------------------------------------------------------------
static void pin_task_to_core_(TaskHandle_t handle, UBaseType_t core) {
#if defined(configUSE_CORE_AFFINITY) && (configUSE_CORE_AFFINITY == 1) && (configNUM_CORES > 1)
  vTaskCoreAffinitySet(handle, static_cast<UBaseType_t>(1u << core));
#else
  (void) handle;
  (void) core;
#endif
}

// ---------------------------------------------------------------------------
// Task bodies
// ---------------------------------------------------------------------------

void LinBusListener::lin_uart_task_(void *args) {
  if (args == nullptr) { vTaskDelete(nullptr); return; }
  LinBusListener *self = static_cast<LinBusListener *>(args);
  for (;;) {
    if (self->is_failed()) {
      ESP_LOGW(TAG, "lin_uart_task_: component failed — task exiting.");
      self->lin_uart_task_handle_ = nullptr;
      vTaskDelete(nullptr);
      return;
    }
    uint32_t delay_ms = self->onSerialEvent();
    if (delay_ms == 0) delay_ms = 1;
    if (delay_ms > 1000) delay_ms = 1000;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }
  vTaskDelete(nullptr);
}

void LinBusListener::lin_event_task_(void *args) {
  if (args == nullptr) { vTaskDelete(nullptr); return; }
  LinBusListener *self = static_cast<LinBusListener *>(args);
  for (;;) {
    if (self->is_failed()) {
      ESP_LOGW(TAG, "lin_event_task_: component failed — task exiting.");
      self->lin_event_task_handle_ = nullptr;
      vTaskDelete(nullptr);
      return;
    }
    // Blocks until a complete LIN frame is available.
    self->process_lin_msg_queue(QUEUE_WAIT_BLOCKING);
  }
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// setup_framework() — called once from LinBusListener::setup() on Core 0
// ---------------------------------------------------------------------------
void LinBusListener::setup_framework() {
  TRUMA_GUARD_NOT_NULL(this->parent_, "parent UART in setup_framework",
                       this->mark_failed(); return);

  auto *uart_comp = static_cast<ESPHOME_UART *>(this->parent_);

  // Guard: only hardware UART supported — SerialPIO cannot detect BREAK.
  if (!uart_comp->is_hw_serial()) {
    guards_detail::record_error(this->error_count_, this->last_error_,
                                TrumaErrorCode::ERR_RP2040_NO_HW_SERIAL);
    ESP_LOGW(TAG, "Guard[HW_SERIAL]: LIN requires hardware UART — SerialPIO not supported.");
    this->mark_failed();
    return;
  }

  HardwareSerial *hw_serial = uart_comp->get_hw_serial();

  // Guard: must be Serial1 (uart0) or Serial2 (uart1).
  if (*hw_serial == Serial1) {
    this->uart_number_ = 1;
    this->uart_ = uart0;
  } else if (*hw_serial == Serial2) {
    this->uart_number_ = 2;
    this->uart_ = uart1;
  } else {
    guards_detail::record_error(this->error_count_, this->last_error_,
                                TrumaErrorCode::ERR_RP2040_UNKNOWN_SERIAL);
    ESP_LOGE(TAG, "Guard[UNKNOWN_SERIAL]: unknown SerialUART instance.");
    this->mark_failed();
    return;
  }

  // Disable FIFO: every received byte triggers an event immediately.
  uart_set_fifo_enabled(this->uart_, false);

  g_lin_listener_for_stack_hook = this;

  // ---- UART polling task (high priority) ---------------------------------
  BaseType_t ret = xTaskCreate(
      LinBusListener::lin_uart_task_, "lin_uart",
      TRUMA_LIN_UART_TASK_STACK_SIZE, this,
      24,   // same priority as ESP32 uartEventTask_
      &this->lin_uart_task_handle_);

  if (ret != pdPASS || this->lin_uart_task_handle_ == nullptr) {
    guards_detail::record_error(this->error_count_, this->last_error_,
                                TrumaErrorCode::ERR_RP2040_TASK_CREATE);
    ESP_LOGE(TAG, "Guard[TASK_CREATE]: failed to create lin_uart task (OOM?).");
    g_lin_listener_for_stack_hook = nullptr;
    this->mark_failed();
    return;
  }
  pin_task_to_core_(this->lin_uart_task_handle_, TRUMA_LIN_TASK_CORE);

  // ---- Event dispatch task (low priority) --------------------------------
  ret = xTaskCreate(
      LinBusListener::lin_event_task_, "lin_event",
      TRUMA_LIN_EVENT_TASK_STACK_SIZE, this,
      2,    // same priority as ESP32 eventTask_
      &this->lin_event_task_handle_);

  if (ret != pdPASS || this->lin_event_task_handle_ == nullptr) {
    guards_detail::record_error(this->error_count_, this->last_error_,
                                TrumaErrorCode::ERR_RP2040_TASK_CREATE);
    ESP_LOGE(TAG, "Guard[TASK_CREATE]: failed to create lin_event task — cleaning up.");
    vTaskDelete(this->lin_uart_task_handle_);
    this->lin_uart_task_handle_ = nullptr;
    g_lin_listener_for_stack_hook = nullptr;
    this->mark_failed();
    return;
  }
  pin_task_to_core_(this->lin_event_task_handle_, TRUMA_LIN_TASK_CORE);

  ESP_LOGD(TAG, "UART%d: lin_uart (prio 24) + lin_event (prio 2) tasks on Core %d.",
           this->uart_number_, TRUMA_LIN_TASK_CORE);
}

// ---------------------------------------------------------------------------
// onSerialEvent() — called by lin_uart_task_ at high priority on Core 1
// Returns the recommended poll delay in milliseconds.
// ---------------------------------------------------------------------------
uint32_t LinBusListener::onSerialEvent() {
  if (this->is_failed()) return 1000;

  this->onReceive_();

  if (this->uart_ != nullptr) {
    uint32_t rsr = uart_get_hw(this->uart_)->rsr;
    if ((rsr & UART_UARTRSR_BE_BITS) == UART_UARTRSR_BE_BITS) {
      ESP_LOGVV(TAG, "UART%d RX BREAK detected.", this->uart_number_);

      // BUG FIX: original code used || (tautological — always true).
      // Correct intent: only force SYNC state when NOT already in BREAK or SYNC.
      if (this->current_state_ != READ_STATE_BREAK &&
          this->current_state_ != READ_STATE_SYNC) {
        this->current_state_ = READ_STATE_SYNC;
      }

      // Clear the break-error flag in the Receive Status Register.
      hw_clear_bits(&uart_get_hw(this->uart_)->rsr, UART_UARTRSR_BE_BITS);
    }
  }

  if (this->current_state_ == READ_STATE_BREAK) {
    uint32_t now = micros();
    if ((this->last_data_recieved_ + 1000u * 1000u) < now) return 750;
    if ((this->last_data_recieved_ + 50u * 1000u) < now)   return 50;
    return 10;
  }
  return 1;
}

}  // namespace truma_inetbox
}  // namespace esphome

// ---------------------------------------------------------------------------
// FreeRTOS stack-overflow hook (weak symbol override)
//
// The kernel calls this when a task stack overflows.  We report the fault via
// the public on_fatal_error_() method (error_count_/last_error_ are protected
// and inaccessible from an extern "C" function) then halt so the watchdog can
// reset the MCU.
// ---------------------------------------------------------------------------
extern "C" void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
  using esphome::truma_inetbox::g_lin_listener_for_stack_hook;
  static const char *const HOOK_TAG = "truma_inetbox.LinBusListener";

  ESP_LOGE(HOOK_TAG, "FATAL: stack overflow in task '%s' — system halted.",
           pcTaskName ? pcTaskName : "?");

  if (g_lin_listener_for_stack_hook != nullptr) {
    g_lin_listener_for_stack_hook->on_fatal_error_(
        esphome::truma_inetbox::TrumaErrorCode::ERR_RP2040_STACK_OVERFLOW);
  }
  for (;;) { tight_loop_contents(); }
}

#undef QUEUE_WAIT_DONT_BLOCK
#undef QUEUE_WAIT_BLOCKING
#undef ESPHOME_UART

#endif  // USE_RP2040
