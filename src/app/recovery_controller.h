#pragma once

#include "listopad/session.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace listopad::app {

class RecoveryController final {
 public:
  explicit RecoveryController(SessionStore store);
  RecoveryController(SessionStore store, bool start_worker);
  ~RecoveryController();
  RecoveryController(const RecoveryController&) = delete;
  RecoveryController& operator=(const RecoveryController&) = delete;

  void start();
  void save(RecoverySnapshot snapshot);
  void remove(std::string id);
  void save_manifest(SessionManifest manifest);
  void flush();

  [[nodiscard]] bool consume_write_error() noexcept {
    return write_error_.exchange(false);
  }

 private:
  void run(std::stop_token stop);

  SessionStore store_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::condition_variable flushed_;
  std::unordered_map<std::string, std::optional<RecoverySnapshot>>
      recovery_jobs_;
  std::optional<SessionManifest> manifest_job_;
  std::size_t active_jobs_{0};
  std::atomic_bool write_error_{false};
  // Keep the worker last: its entry point may access every field above as soon
  // as construction starts.
  std::jthread worker_;
};

}  // namespace listopad::app
