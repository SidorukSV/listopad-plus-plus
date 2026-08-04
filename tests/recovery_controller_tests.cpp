#include "recovery_controller.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <string>

using namespace listopad;
using namespace listopad::app;

namespace {

class RecoveryTestDirectory final {
 public:
  RecoveryTestDirectory() {
    std::wstring root(32768, L'\0');
    const DWORD length =
        GetTempPathW(static_cast<DWORD>(root.size()), root.data());
    root.resize(length);
    path_ = std::filesystem::path(root) /
            (L"ListopadPP.recovery-controller-tests." +
             std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path_);
  }

  ~RecoveryTestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("recovery controller coalesces pending generations by tab id") {
  const RecoveryTestDirectory directory;
  const SessionStore store(directory.path());
  RecoveryController controller(SessionStore(directory.path()));

  RecoverySnapshot first;
  first.id = "0123456789abcdef";
  first.title = L"first";
  first.content = "first";
  RecoverySnapshot latest = first;
  latest.title = L"latest";
  latest.content = "latest";

  controller.save(std::move(first));
  controller.save(std::move(latest));
  controller.flush();

  const auto loaded =
      store.load_recovery(store.recovery_path("0123456789abcdef"));
  REQUIRE(loaded.status == StoreLoadStatus::Loaded);
  CHECK(loaded.value.content == "latest");
}

TEST_CASE("recovery controller persists manifest and removes snapshots") {
  const RecoveryTestDirectory directory;
  const SessionStore store(directory.path());
  RecoveryController controller(SessionStore(directory.path()));

  RecoverySnapshot snapshot;
  snapshot.id = "fedcba9876543210";
  snapshot.title = L"draft";
  snapshot.content = "draft";
  controller.save(snapshot);
  controller.flush();

  SessionManifest manifest;
  manifest.clean_shutdown = false;
  manifest.active_index = 0;
  controller.save_manifest(manifest);
  controller.remove(snapshot.id);
  controller.flush();

  CHECK(store.load_manifest().status == StoreLoadStatus::Loaded);
  CHECK(store.load_recovery(store.recovery_path(snapshot.id)).status ==
        StoreLoadStatus::Missing);
}
