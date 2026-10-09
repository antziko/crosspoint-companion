#pragma once
// Host-test stub for lib/Logging/Logging.h (device-only — includes Arduino
// HardwareSerial). RecentBooksStore logs I/O failures; on the host
// these are no-ops.
#define LOG_ERR(origin, format, ...) ((void)0)
#define LOG_INF(origin, format, ...) ((void)0)
#define LOG_DBG(origin, format, ...) ((void)0)
