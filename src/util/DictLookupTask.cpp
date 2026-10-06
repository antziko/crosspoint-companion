#include "DictLookupTask.h"

#include <SdDebugLog.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "DictionaryLookupController.h"

void DictLookupTask::run() {
  owner.runLookup();
  // Peak stack use of the 4 KB task, including any SD log lines written during the lookup.
  // ESP-IDF reports this in bytes.
  SdDebugLog::log("DICT", "lookup task stack free=%u", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}
