#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include <sdkconfig.h>

// Heap-trace fields for the core settings platformio.ini's custom_sdkconfig tunes.
namespace SysTaskStacks {

// Unused stack bytes at the deepest point so far (ESP-IDF counts stack in bytes);
// 0 when the task is not running.
inline unsigned espTimerHeadroom() {
  const TaskHandle_t task = xTaskGetHandle("esp_timer");
  return task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)) : 0;
}

inline unsigned timerServiceHeadroom() {
  const TaskHandle_t task = xTimerGetTimerDaemonTaskHandle();
  return task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)) : 0;
}

#ifdef CONFIG_ESP_WIFI_IRAM_OPT
constexpr int WIFI_IRAM = 1;
#else
constexpr int WIFI_IRAM = 0;
#endif
#ifdef CONFIG_ESP_WIFI_RX_IRAM_OPT
constexpr int WIFI_RX_IRAM = 1;
#else
constexpr int WIFI_RX_IRAM = 0;
#endif

}  // namespace SysTaskStacks
