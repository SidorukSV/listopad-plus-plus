#include "recovery_controller.h"

#include <utility>

namespace listopad::app {

RecoveryController::RecoveryController(SessionStore store)
    : RecoveryController(std::move(store), true) {}

RecoveryController::RecoveryController(SessionStore store,
                                       const bool start_worker)
    : store_(std::move(store)) {
  if (start_worker) start();
}

RecoveryController::~RecoveryController() {
  if (!worker_.joinable()) return;
  flush();
  worker_.request_stop();
  condition_.notify_all();
}

void RecoveryController::start() {
  if (worker_.joinable()) return;
  worker_ = std::jthread(
      [this](const std::stop_token stop) { run(stop); });
}

void RecoveryController::save(RecoverySnapshot snapshot) {
  if (!valid_session_id(snapshot.id)) return;
  {
    std::lock_guard lock(mutex_);
    recovery_jobs_[snapshot.id] = std::move(snapshot);
  }
  condition_.notify_one();
}

void RecoveryController::remove(std::string id) {
  if (!valid_session_id(id)) return;
  {
    std::lock_guard lock(mutex_);
    recovery_jobs_[id] = std::nullopt;
  }
  condition_.notify_one();
}

void RecoveryController::save_manifest(SessionManifest manifest) {
  {
    std::lock_guard lock(mutex_);
    manifest_job_ = std::move(manifest);
  }
  condition_.notify_one();
}

void RecoveryController::flush() {
  if (!worker_.joinable()) return;
  std::unique_lock lock(mutex_);
  flushed_.wait(lock, [this] {
    return recovery_jobs_.empty() && !manifest_job_ &&
           active_jobs_ == 0;
  });
}

void RecoveryController::run(const std::stop_token stop) {
  for (;;) {
    std::optional<SessionManifest> manifest;
    std::string id;
    std::optional<RecoverySnapshot> recovery;
    {
      std::unique_lock lock(mutex_);
      condition_.wait(lock, [this, stop] {
        return stop.stop_requested() || manifest_job_.has_value() ||
               !recovery_jobs_.empty();
      });
      if (stop.stop_requested() && !manifest_job_ &&
          recovery_jobs_.empty()) {
        break;
      }
      if (manifest_job_) {
        manifest = std::move(manifest_job_);
        manifest_job_.reset();
      } else {
        auto job = recovery_jobs_.begin();
        id = job->first;
        recovery = std::move(job->second);
        recovery_jobs_.erase(job);
      }
      ++active_jobs_;
    }

    bool ok = false;
    if (manifest) {
      ok = store_.save_manifest(*manifest);
    } else if (recovery) {
      ok = store_.save_recovery(*recovery);
    } else {
      ok = store_.remove_recovery(id);
    }
    if (!ok) write_error_ = true;

    {
      std::lock_guard lock(mutex_);
      --active_jobs_;
      if (recovery_jobs_.empty() && !manifest_job_ &&
          active_jobs_ == 0) {
        flushed_.notify_all();
      }
    }
  }
}

}  // namespace listopad::app
