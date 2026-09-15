#pragma once

// SIGPIPE handling.
//
// Writing to a socket whose peer has gone away raises SIGPIPE, and its default
// disposition terminates the process (observed as exit code 141 = 128 + 13).
// The SDK already prevents this for its own connections without touching any
// process-wide state:
//
//   * macOS/BSD: SO_NOSIGPIPE is set on every socket the SDK opens, which
//     covers plaintext and TLS writes alike.
//   * Linux, plaintext: writes go through send(..., MSG_NOSIGNAL).
//   * Linux, TLS: SSL_write/SSL_read cannot carry MSG_NOSIGNAL, so they run
//     with SIGPIPE blocked on the calling thread; the SDK consumes the pending
//     signal it caused before restoring the previous mask. Signals the host had
//     pending are left alone, and the host's SIGPIPE handler — if any — keeps
//     working for the host's own sockets.
//
// So applications normally need nothing from this header. It exists for hosts
// that prefer the simpler, process-wide policy (the one libcurl and many
// servers adopt): never receive SIGPIPE at all, anywhere.

namespace trtc_asr {

/// Sets the process-wide disposition of SIGPIPE to "ignore", so that failed
/// writes report EPIPE instead of raising a signal.
///
/// Opt-in: the SDK never calls this itself, because it changes state shared by
/// the whole process — including code the SDK does not own. Call it at most
/// once during start-up, and only if the application has no SIGPIPE handler of
/// its own.
///
/// Returns false if the disposition could not be changed (the previous
/// disposition is then left untouched).
bool IgnoreSigpipeProcessWide();

}  // namespace trtc_asr
