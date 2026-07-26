// wolfSSL (Arduino-wolfSSL) references wolfSSL_Arduino_Serial_Print from its
// logging path when built under -DARDUINO (see wolfcrypt/src/logging.c). The
// symbol is *defined* only in the library's Arduino wrapper header (wolfssl.h),
// which no compiled translation unit in this project includes — so the link
// fails with an undefined reference the moment any code actually calls into
// wolfSSL. Provide the definition here, once, with C linkage to match the C
// call site. Routed to LOG_DBG; it only ever fires if wolfSSL debug logging is
// compiled in. Guarded so a non-wolfSSL build omits it entirely.
#if defined(FREEINK_NET_WOLFSSL)
#include <Logging.h>

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }
#endif
