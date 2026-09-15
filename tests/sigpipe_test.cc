#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <pthread.h>
#include <unistd.h>

#include "mock_servers.h"
#include "trtc_asr/sigpipe.h"
#include "ws_client.h"

// Writing to a socket whose peer is gone raises SIGPIPE, and its default
// disposition kills the process (exit code 141). The tests that do cover
// SIGPIPE would abort the whole test binary if the protection regressed — that
// crash IS the failure signal, so they do not assert "did not die" explicitly.
//
// Verified to be discriminating: with both SuppressSigpipeOnSocket and the
// MSG_NOSIGNAL flag disarmed, SendToClosedPeerReportsErrorInsteadOfSignal kills
// the runner instead of failing an assertion.

namespace {

using trtc_asr::internal::ScopedSigpipeSuppressor;
using trtc_asr::internal::SendNoSignal;
using trtc_asr::internal::SuppressSigpipeOnSocket;
using trtc_asr::internal::WsClient;
using trtc_asr_test::MockWsServer;
using trtc_asr_test::MockWsSession;

/// Fails the test when SIGPIPE is not at its default disposition: the tests
/// below would silently pass for the wrong reason (an ambient SIG_IGN) instead
/// of exercising the SDK's own protection.
void RequireDefaultSigpipeDisposition() {
  struct sigaction current;
  ASSERT_EQ(0, sigaction(SIGPIPE, nullptr, &current));
  ASSERT_EQ(SIG_DFL, current.sa_handler)
      << "SIGPIPE is not at its default disposition, so this test cannot prove "
         "anything about the SDK's own SIGPIPE handling";
}

bool SigpipePending() {
  sigset_t pending;
  sigemptyset(&pending);
  EXPECT_EQ(0, sigpending(&pending));
  return sigismember(&pending, SIGPIPE) == 1;
}

}  // namespace

// --- fd-level / per-call protection (all platforms) -------------------------

// The production write path: SuppressSigpipeOnSocket at connect time
// (SO_NOSIGPIPE on macOS/BSD) plus SendNoSignal per write (MSG_NOSIGNAL on
// Linux). Whichever of the two the platform provides must turn a write to a
// departed peer into a plain error.
TEST(SigpipeTest, SendToClosedPeerReportsErrorInsteadOfSignal) {
  RequireDefaultSigpipeDisposition();

  int fds[2];
  ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
  SuppressSigpipeOnSocket(fds[0]);
  ASSERT_EQ(0, close(fds[1]));  // peer is gone

  const std::string payload(1024, 'x');
  ssize_t rc = 0;
  int last_errno = 0;
  // The first write may still be absorbed by the socket buffer; a couple of
  // rounds are enough for the closed peer to surface.
  for (int i = 0; i < 16; i++) {
    errno = 0;
    rc = SendNoSignal(fds[0], payload.data(), payload.size());
    last_errno = errno;
    if (rc < 0) break;
  }
  EXPECT_LT(rc, 0) << "write to a closed peer unexpectedly succeeded";
  EXPECT_TRUE(last_errno == EPIPE || last_errno == ECONNRESET)
      << "unexpected errno " << last_errno;
  EXPECT_FALSE(SigpipePending());

  close(fds[0]);
}

// --- thread-mask guard (Linux, where SSL_write cannot take MSG_NOSIGNAL) -----

#ifdef TRTC_ASR_SIGPIPE_GUARD

// OpenSSL writes through plain write(2), which no per-call flag can reach;
// the guard is what keeps that from killing the host.
TEST(SigpipeTest, GuardTurnsWriteToBrokenPipeIntoEpipe) {
  RequireDefaultSigpipeDisposition();
  ASSERT_TRUE(ScopedSigpipeSuppressor::Active());

  int fds[2];
  ASSERT_EQ(0, pipe(fds));
  ASSERT_EQ(0, close(fds[0]));  // no reader left

  ssize_t rc;
  int last_errno;
  {
    ScopedSigpipeSuppressor no_sigpipe;
    errno = 0;
    rc = ::write(fds[1], "x", 1);
    last_errno = errno;
  }
  EXPECT_EQ(-1, rc);
  EXPECT_EQ(EPIPE, last_errno) << "the guard must not clobber errno";
  // The scope consumed the SIGPIPE it caused, so restoring the mask cannot
  // deliver it afterwards.
  EXPECT_FALSE(SigpipePending());

  close(fds[1]);
}

// A SIGPIPE the host already had pending belongs to the host: the guard must
// leave it alone instead of swallowing it on the way out.
TEST(SigpipeTest, GuardDoesNotConsumeSignalPendingBeforeItsScope) {
  RequireDefaultSigpipeDisposition();

  sigset_t sigpipe_set;
  sigemptyset(&sigpipe_set);
  sigaddset(&sigpipe_set, SIGPIPE);
  sigset_t old_mask;
  ASSERT_EQ(0, pthread_sigmask(SIG_BLOCK, &sigpipe_set, &old_mask));

  ASSERT_EQ(0, raise(SIGPIPE));
  ASSERT_TRUE(SigpipePending());

  {
    ScopedSigpipeSuppressor no_sigpipe;
    int fds[2];
    ASSERT_EQ(0, pipe(fds));
    ASSERT_EQ(0, close(fds[0]));
    ::write(fds[1], "x", 1);
    close(fds[1]);
  }

  EXPECT_TRUE(SigpipePending()) << "the host's pending SIGPIPE was stolen";

  // Clean up: drain it ourselves and restore the mask.
  struct timespec zero = {0, 0};
  EXPECT_EQ(SIGPIPE, sigtimedwait(&sigpipe_set, nullptr, &zero));
  EXPECT_EQ(0, pthread_sigmask(SIG_SETMASK, &old_mask, nullptr));
}

// The guard restores the mask it found, including the "already blocked" case.
TEST(SigpipeTest, GuardRestoresPreviousSignalMask) {
  sigset_t sigpipe_set;
  sigemptyset(&sigpipe_set);
  sigaddset(&sigpipe_set, SIGPIPE);

  sigset_t before;
  sigemptyset(&before);
  ASSERT_EQ(0, pthread_sigmask(SIG_SETMASK, nullptr, &before));
  ASSERT_EQ(0, sigismember(&before, SIGPIPE));

  {
    ScopedSigpipeSuppressor no_sigpipe;
    sigset_t inside;
    sigemptyset(&inside);
    ASSERT_EQ(0, pthread_sigmask(SIG_SETMASK, nullptr, &inside));
    EXPECT_EQ(1, sigismember(&inside, SIGPIPE));
  }

  sigset_t after;
  sigemptyset(&after);
  ASSERT_EQ(0, pthread_sigmask(SIG_SETMASK, nullptr, &after));
  EXPECT_EQ(0, sigismember(&after, SIGPIPE));
}

#endif  // TRTC_ASR_SIGPIPE_GUARD

// --- end to end -------------------------------------------------------------

// A server that hangs up mid-session must surface as a write error.
//
// Note on scope: this does NOT exercise SIGPIPE. WriteAll polls for POLLOUT
// first, and once the peer's RST has been processed poll reports POLLERR, so
// the write is abandoned before send/SSL_write is ever reached. SIGPIPE only
// appears in the race where poll still reports the socket writable and the
// reset lands in between — which is exactly why it shows up as a rare crash in
// production and why the protection is covered by the unit tests above instead
// of here. What this test pins down is the other half: the hang-up is reported
// as an error rather than being silently swallowed.
TEST(SigpipeTest, WsWriteAfterServerHangUpReportsError) {
  RequireDefaultSigpipeDisposition();

  MockWsServer server([](MockWsSession& session) { session.Close(); });

  std::string err;
  auto client = WsClient::Connect(server.Url() + "/asr/v2", 5000, &err);
  ASSERT_NE(nullptr, client) << err;
  server.Join();  // the peer is definitely gone now

  const std::vector<uint8_t> chunk(4096, 0xAB);
  bool write_failed = false;
  for (int i = 0; i < 64 && !write_failed; i++) {
    std::string write_err;
    if (!client->SendBinary(chunk.data(), chunk.size(), &write_err)) {
      write_failed = true;
      EXPECT_FALSE(write_err.empty());
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  EXPECT_TRUE(write_failed) << "writes to a hung-up server never reported an error";
}

// --- opt-in process-wide policy ---------------------------------------------

TEST(SigpipeTest, IgnoreSigpipeProcessWideSetsIgnoreDisposition) {
  struct sigaction previous;
  ASSERT_EQ(0, sigaction(SIGPIPE, nullptr, &previous));

  EXPECT_TRUE(trtc_asr::IgnoreSigpipeProcessWide());

  struct sigaction current;
  ASSERT_EQ(0, sigaction(SIGPIPE, nullptr, &current));
  EXPECT_EQ(SIG_IGN, current.sa_handler);

  EXPECT_EQ(0, sigaction(SIGPIPE, &previous, nullptr));
}
