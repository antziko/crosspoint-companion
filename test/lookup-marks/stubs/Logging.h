#pragma once
// Host-test stub for lib/Logging/Logging.h (device-only — includes Arduino
// HardwareSerial). FlashcardDeck only uses LOG_ERR on I/O failure; on the host
// these are no-ops.
#define LOG_ERR(origin, format, ...) ((void)0)
#define LOG_INF(origin, format, ...) ((void)0)
#define LOG_DBG(origin, format, ...) ((void)0)
