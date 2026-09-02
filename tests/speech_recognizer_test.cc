#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mock_servers.h"
#include "trtc_asr/errors.h"
#include "trtc_asr/speech_recognizer.h"

namespace {

using trtc_asr::ASRError;
using trtc_asr::SpeechRecognitionResponse;
using trtc_asr::SpeechRecognizer;
using trtc_asr_test::MockWsServer;
using trtc_asr_test::MockWsSession;

trtc_asr::Credential TestCredential() {
  return trtc_asr::Credential(1300000000, 1400000000, "test-secret");
}

/// A listener that records events for assertions.
class RecordingListener : public trtc_asr::SpeechRecognitionListener {
 public:
  void OnRecognitionStart(const SpeechRecognitionResponse& r) override {
    Add("start:" + r.voice_id);
  }
  void OnSentenceBegin(const SpeechRecognitionResponse& r) override {
    Add("begin:" + std::to_string(r.result.index));
  }
  void OnRecognitionResultChange(const SpeechRecognitionResponse& r) override {
    Add("change:" + r.result.voice_text_str);
  }
  void OnSentenceEnd(const SpeechRecognitionResponse& r) override {
    Add("end:" + r.result.voice_text_str);
  }
  void OnRecognitionComplete(const SpeechRecognitionResponse& r) override {
    Add("complete:" + std::to_string(r.final_flag));
  }
  void OnFail(const SpeechRecognitionResponse* r, const ASRError& e) override {
    Add("fail:" + std::to_string(e.code()));
    last_error = std::make_unique<ASRError>(e);
    if (r != nullptr) last_fail_response = *r;
  }

  void Add(const std::string& event) {
    std::lock_guard<std::mutex> lock(mu);
    events.push_back(event);
    cv.notify_all();
  }

  int Count(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mu);
    int n = 0;
    for (const auto& e : events) {
      if (e.rfind(prefix, 0) == 0) n++;
    }
    return n;
  }

  bool WaitFor(const std::string& prefix, int count, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mu);
    return cv.wait_until(lock, deadline, [&] {
      int n = 0;
      for (const auto& e : events) {
        if (e.rfind(prefix, 0) == 0) n++;
      }
      return n >= count;
    });
  }

  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::string> events;
  std::unique_ptr<ASRError> last_error;
  SpeechRecognitionResponse last_fail_response;
};

std::unique_ptr<SpeechRecognizer> NewRecognizer(
    trtc_asr::SpeechRecognitionListener* l, const std::string& ws_url) {
  auto r = std::make_unique<SpeechRecognizer>(TestCredential(), "16k_zh_en", l);
  r->SetEndpoint(ws_url);
  r->SetWriteTimeout(std::chrono::milliseconds(500));
  r->SetStopTimeout(std::chrono::milliseconds(2000));
  return r;
}

/// A server handler that holds the session open and answers the end signal
/// with a final frame, so Stop returns promptly.
MockWsServer::Handler HoldOpenAnsweringEnd() {
  return [](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  };
}

void ExpectErrorCode(const std::function<void()>& fn, int code) {
  try {
    fn();
    FAIL() << "expected ASRError " << code;
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), code) << e.what();
  }
}

TEST(SpeechRecognizer, WriteBeforeStartReturnsNotStarted) {
  MockWsServer server([](MockWsSession&) {});
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  ExpectErrorCode([&] { r->Write({1, 2, 3}); }, trtc_asr::kErrNotStarted);
}

TEST(SpeechRecognizer, StopBeforeStartReturnsNotStarted) {
  MockWsServer server([](MockWsSession&) {});
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  ExpectErrorCode([&] { r->Stop(); }, trtc_asr::kErrNotStarted);
}

TEST(SpeechRecognizer, StartTwiceReturnsAlreadyStarted) {
  MockWsServer server(HoldOpenAnsweringEnd());
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->Start();
  ExpectErrorCode([&] { r->Start(); }, trtc_asr::kErrAlreadyStarted);
  r->Stop();
  server.Join();
}

TEST(SpeechRecognizer, StartRejectsInvalidOptionsAndStaysReusable) {
  MockWsServer server(HoldOpenAnsweringEnd());
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());

  r->SetSpeakerDiarization(2);  // unsupported
  try {
    r->Start();
    FAIL() << "expected invalid param error";
  } catch (const ASRError& e) {
    EXPECT_EQ(e.code(), trtc_asr::kErrInvalidParam);
    EXPECT_NE(std::string(e.what()).find("SpeakerDiarization must be 0"),
              std::string::npos);
  }

  // A rejected start leaves the recognizer reusable after fixing options.
  r->SetSpeakerDiarization(0);
  r->Start();
  r->Stop();
  server.Join();
}

TEST(SpeechRecognizer, StartRejectsOutOfRangeNoiseThreshold) {
  MockWsServer server([](MockWsSession&) {});
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->SetNoiseThreshold(5.0);
  try {
    r->Start();
    FAIL() << "expected invalid param error";
  } catch (const ASRError& e) {
    EXPECT_NE(std::string(e.what()).find("NoiseThreshold must be between"),
              std::string::npos);
  }
}

TEST(SpeechRecognizer, StartRejectsRolesWithoutVoiceprintMode) {
  MockWsServer server([](MockWsSession&) {});
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->SetSpeakerDiarization(1);
  r->SetSpeakerRoles({{"teacher", "https://example.com/a.wav"}});
  try {
    r->Start();
    FAIL() << "expected invalid param error";
  } catch (const ASRError& e) {
    EXPECT_NE(std::string(e.what()).find("require SpeakerDiarization=3"),
              std::string::npos);
  }
}

TEST(SpeechRecognizer, HandshakeSendsAuthQueryParams) {
  MockWsServer server(HoldOpenAnsweringEnd());
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->SetVoiceId("voice-handshake");
  r->Start();

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (server.RequestTarget().empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::string target = server.RequestTarget();
  ASSERT_FALSE(target.empty());
  EXPECT_TRUE(target.rfind("/asr/v2/1300000000?", 0) == 0) << target;
  for (const char* key :
       {"sdkappid=1400000000", "usersig=", "signature=", "secretid=1300000000",
        "voice_id=voice-handshake", "engine_model_type=16k_zh_en", "timestamp=",
        "expired=", "nonce="}) {
    EXPECT_NE(target.find(key), std::string::npos) << "missing " << key;
  }
  // SecretKey must never reach the wire.
  EXPECT_EQ(target.find("test-secret"), std::string::npos);

  r->Stop();
  server.Join();
}

TEST(SpeechRecognizer, FullSessionLifecycle) {
  RecordingListener listener;
  MockWsServer server([](MockWsSession& s) {
    // Handshake ack: no result object — must not trigger sentence begin.
    s.SendText(R"({"code":0,"message":"success","voice_id":"v1"})");
    s.SendText(R"({"code":0,"message":"success","voice_id":"v1","message_id":"m1","result":{"slice_type":0,"index":0,"voice_text_str":"今天。"}})");
    s.SendText(R"({"code":0,"message":"success","voice_id":"v1","message_id":"m2","result":{"slice_type":1,"index":0,"voice_text_str":"今天天气"}})");
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(R"({"code":0,"message":"success","voice_id":"v1","message_id":"m3","final":1,"result":{"slice_type":2,"index":0,"voice_text_str":"今天天气不错。"}})");
        return;
      }
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->SetVoiceId("v1");
  r->Start();
  r->Write({0, 1, 2, 3});
  r->Stop();

  EXPECT_TRUE(listener.WaitFor("complete:", 1, 2000));
  EXPECT_EQ(listener.Count("start:"), 1);
  // The handshake ack must NOT be dispatched as sentence begin.
  EXPECT_EQ(listener.Count("begin:"), 1);
  EXPECT_EQ(listener.Count("change:"), 1);
  EXPECT_EQ(listener.Count("end:"), 1);
  EXPECT_EQ(listener.Count("complete:"), 1);
  EXPECT_EQ(listener.Count("fail:"), 0);

  // A late write must report not-running.
  ExpectErrorCode([&] { r->Write({9}); }, trtc_asr::kErrNotStarted);

  server.Join();
}

TEST(SpeechRecognizer, WriteAndEndSignalReachServer) {
  auto got_audio = std::make_shared<std::atomic<bool>>(false);
  auto got_end = std::make_shared<std::atomic<bool>>(false);
  MockWsServer server([got_audio, got_end](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x2 && payload == std::string("\x01\x02\x03", 3)) {
        got_audio->store(true);
      }
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        got_end->store(true);
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->Start();
  r->Write({1, 2, 3});
  r->Stop();

  EXPECT_TRUE(got_audio->load()) << "server should receive the audio frame";
  EXPECT_TRUE(got_end->load()) << "server should receive the end signal";
  server.Join();
}

TEST(SpeechRecognizer, ServerErrorTriggersOnFailAndStops) {
  RecordingListener listener;
  MockWsServer server([](MockWsSession& s) {
    s.SendText(R"({"code":4006,"message":"quota exceeded","voice_id":"v1","result":{}})");
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->Start();

  EXPECT_TRUE(listener.WaitFor("fail:", 1, 2000));
  ASSERT_TRUE(listener.last_error != nullptr);
  EXPECT_EQ(listener.last_error->code(), 4006);

  // After a terminal error the recognizer is stopped: late writes fail.
  ExpectErrorCode([&] { r->Write({9}); }, trtc_asr::kErrNotStarted);
  ExpectErrorCode([&] { r->Stop(); }, trtc_asr::kErrNotStarted);
  server.Join();
}

TEST(SpeechRecognizer, FinalWithSliceZeroOnlyCompletes) {
  RecordingListener listener;
  MockWsServer server([](MockWsSession& s) {
    s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":0,"index":0}})");
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->Start();

  EXPECT_TRUE(listener.WaitFor("complete:", 1, 2000));
  EXPECT_EQ(listener.Count("begin:"), 0);
  EXPECT_EQ(listener.Count("end:"), 0);
  EXPECT_EQ(listener.Count("complete:"), 1);
  server.Join();
}

TEST(SpeechRecognizer, MalformedFrameIsNonTerminal) {
  RecordingListener listener;
  MockWsServer server([](MockWsSession& s) {
    s.SendText("not-json");
    s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","result":{"slice_type":1,"index":0,"voice_text_str":"hi"}})");
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->Start();

  // The session continues past the malformed frame.
  EXPECT_TRUE(listener.WaitFor("change:", 1, 2000));
  EXPECT_EQ(listener.Count("fail:" + std::to_string(trtc_asr::kErrReadFailed)), 1);

  r->Stop();
  server.Join();
}

/// A listener that calls Stop() from a non-terminal callback. Re-entry must
/// return promptly (the callback runs on the reader thread, so waiting for
/// the terminal response there would self-deadlock).
class StopFromChangeListener : public RecordingListener {
 public:
  void OnRecognitionResultChange(const SpeechRecognitionResponse& r) override {
    RecordingListener::OnRecognitionResultChange(r);
    auto start = std::chrono::steady_clock::now();
    try {
      recognizer->Stop();
    } catch (const ASRError& e) {
      stop_error = e.code();
    }
    stop_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
    Add("stopped");
  }

  SpeechRecognizer* recognizer = nullptr;
  std::atomic<long> stop_duration_ms{-1};
  std::atomic<int> stop_error{0};
};

TEST(SpeechRecognizer, StopFromNonTerminalCallbackReturnsPromptly) {
  StopFromChangeListener listener;
  MockWsServer server([](MockWsSession& s) {
    s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","result":{"slice_type":1,"index":0,"voice_text_str":"partial"}})");
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  listener.recognizer = r.get();
  r->Start();

  EXPECT_TRUE(listener.WaitFor("stopped", 1, 3000))
      << "Stop() inside OnRecognitionResultChange did not return promptly "
         "(self-deadlock?)";
  EXPECT_LT(listener.stop_duration_ms.load(), 2000)
      << "Stop took " << listener.stop_duration_ms.load() << "ms, want prompt return";
  EXPECT_EQ(listener.stop_error.load(), 0) << "Stop should succeed";

  // The watchdog completes the session after the server replies final.
  EXPECT_TRUE(listener.WaitFor("complete:", 1, 3000));
  server.Join();
}

/// A listener that throws inside a callback; must not crash the SDK.
class ThrowingListener : public RecordingListener {
 public:
  void OnRecognitionResultChange(const SpeechRecognitionResponse&) override {
    throw std::runtime_error("listener boom");
  }
};

TEST(SpeechRecognizer, ListenerExceptionIsRecoveredAndReported) {
  ThrowingListener listener;
  MockWsServer server([](MockWsSession& s) {
    s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","result":{"slice_type":1,"index":0,"voice_text_str":"hi"}})");
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->Start();

  EXPECT_TRUE(listener.WaitFor("fail:", 1, 2000))
      << "panic should be surfaced via OnFail";
  ASSERT_TRUE(listener.last_error != nullptr);
  EXPECT_EQ(listener.last_error->code(), trtc_asr::kErrReadFailed);
  EXPECT_NE(std::string(listener.last_error->what()).find("listener boom"),
            std::string::npos);

  // The recognizer is stopped after the panic; late writes fail.
  ExpectErrorCode([&] { r->Write({9}); }, trtc_asr::kErrNotStarted);
  server.Join();
}

TEST(SpeechRecognizer, WriteNullDataIsInvalidParam) {
  MockWsServer server(HoldOpenAnsweringEnd());
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->Start();
  ExpectErrorCode([&] { r->Write(nullptr, 1); }, trtc_asr::kErrInvalidParam);
  r->Stop();
  server.Join();
}

/// The terminal frame arrives during Stop()'s timed wait, but the terminal
/// callback runs LONGER than the stop timeout. Stop() must not return early:
/// once the terminal response is in, it waits for the callbacks to finish
/// (mirrors the Go SDK's waitForCallbacksOrAbort terminal branch).
TEST(SpeechRecognizer, StopWaitsForSlowTerminalCallbackBeyondTimeout) {
  class SlowCompleteListener : public RecordingListener {
   public:
    void OnRecognitionComplete(const SpeechRecognitionResponse& r) override {
      RecordingListener::OnRecognitionComplete(r);
      entered.store(true);
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
  };

  SlowCompleteListener listener;
  MockWsServer server([](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->SetStopTimeout(std::chrono::milliseconds(1000));  // callback outlives this
  r->Start();

  std::atomic<bool> stop_returned{false};
  std::thread stop_thread([&] {
    r->Stop();
    stop_returned.store(true);
  });

  // Wait until the terminal callback is running, then let it exceed the
  // stop timeout.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!listener.entered.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  bool callback_running = listener.entered.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_TRUE(callback_running);
  if (callback_running) {
    EXPECT_FALSE(stop_returned.load())
        << "Stop returned while the terminal callback was still running past "
           "stop timeout";
  }

  listener.release.store(true);
  stop_thread.join();
  EXPECT_TRUE(stop_returned.load());
  server.Join();
}

TEST(SpeechRecognizer, StopTimesOutAndForceClosesWhenServerNeverFinishes) {
  RecordingListener listener;
  auto server_got_end = std::make_shared<std::atomic<bool>>(false);
  MockWsServer server([server_got_end](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        server_got_end->store(true);
        // Never reply with a final frame; the client must force-close.
      }
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->SetStopTimeout(std::chrono::milliseconds(1000));
  r->Start();

  auto start = std::chrono::steady_clock::now();
  r->Stop();
  long elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  EXPECT_TRUE(server_got_end->load()) << "server should have read the end signal";
  EXPECT_GE(elapsed_ms, 900);
  EXPECT_LT(elapsed_ms, 5000) << "Stop should wait ~stop_timeout";
  EXPECT_EQ(listener.Count("complete:"), 0);
  server.Join();
}

TEST(SpeechRecognizer, ExternalStopWaitsForTerminalCallback) {
  class BlockingCompleteListener : public RecordingListener {
   public:
    void OnRecognitionComplete(const SpeechRecognitionResponse& r) override {
      RecordingListener::OnRecognitionComplete(r);
      entered.store(true);
      while (!release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
  };

  BlockingCompleteListener listener;
  MockWsServer server([](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  auto r = NewRecognizer(&listener, server.Url());
  r->Start();

  std::atomic<bool> stop_returned{false};
  std::thread stop_thread([&] {
    try {
      r->Stop();
    } catch (const ASRError&) {
    }
    stop_returned.store(true);
  });

  // Wait until the terminal callback is running. Always release + join so a
  // failure here cannot leave threads hanging.
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!listener.entered.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  bool callback_running = listener.entered.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(callback_running);
  if (callback_running) {
    EXPECT_FALSE(stop_returned.load())
        << "Stop returned before terminal callback finished";
  }

  listener.release.store(true);
  stop_thread.join();
  EXPECT_TRUE(stop_returned.load());
  server.Join();
}

TEST(SpeechRecognizer, ReconnectRequiresNewInstance) {
  MockWsServer server(HoldOpenAnsweringEnd());
  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->Start();
  r->Stop();
  ExpectErrorCode([&] { r->Start(); }, trtc_asr::kErrAlreadyStarted);
  server.Join();
}

TEST(SpeechRecognizer, ConcurrentWritesAndStopDoNotDeadlock) {
  auto got_end = std::make_shared<std::atomic<bool>>(false);
  MockWsServer server([got_end](MockWsSession& s) {
    int opcode;
    std::string payload;
    while (s.Read(&opcode, &payload)) {
      if (opcode == 0x1 && payload == R"({"type":"end"})") {
        got_end->store(true);
        s.SendText(R"({"code":0,"message":"ok","voice_id":"v1","final":1,"result":{"slice_type":2}})");
        return;
      }
    }
  });

  RecordingListener listener;
  auto r = NewRecognizer(&listener, server.Url());
  r->Start();

  std::vector<std::thread> writers;
  for (int i = 0; i < 4; i++) {
    writers.emplace_back([&] {
      for (int j = 0; j < 25; j++) {
        try {
          r->Write({0, 1});
        } catch (const ASRError&) {
          // Session may have been stopped concurrently.
        }
      }
    });
  }
  for (auto& w : writers) w.join();
  r->Stop();
  EXPECT_TRUE(got_end->load());
  server.Join();
}

}  // namespace
