#include "trtc_asr/sigpipe.h"

#include <csignal>

namespace trtc_asr {

bool IgnoreSigpipeProcessWide() {
  // sigaction over signal(): signal()'s semantics vary across platforms, and
  // an explicitly zeroed sa_mask / sa_flags keeps the outcome predictable.
  struct sigaction sa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sa.sa_handler = SIG_IGN;
  return sigaction(SIGPIPE, &sa, nullptr) == 0;
}

}  // namespace trtc_asr
