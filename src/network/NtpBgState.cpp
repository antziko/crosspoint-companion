#include "NtpBgState.h"

namespace NtpBg {
volatile bool active = false;
volatile bool cancel = false;
volatile bool startedThisBoot = false;
}  // namespace NtpBg
