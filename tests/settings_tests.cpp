#include "listopad/settings.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <string>

using namespace listopad;

namespace {

class ProfileOverride final {
 public:
  ProfileOverride() {
    std::wstring root(32768, L'\0');
    const DWORD length =
        GetTempPathW(static_cast<DWORD>(root.size()), root.data());
    root.resize(length);
    path_ = std::filesystem::path(root) /
            (L"ListopadPP.settings-tests." +
             std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path_);
    SetEnvironmentVariableW(L"LISTOPAD_PROFILE_DIR", path_.c_str());
  }

  ~ProfileOverride() {
    SetEnvironmentVariableW(L"LISTOPAD_PROFILE_DIR", nullptr);
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

TEST_CASE("settings use the shared profile and round trip recovery policy") {
  const ProfileOverride profile;
  CHECK(profile_directory() == profile.path());
  CHECK(settings_path() == profile.path() / L"settings.json");

  Settings source;
  source.recovery_enabled = false;
  source.restore_session = false;
  source.recovery_max_bytes = 32ull << 20;
  source.recovery_retention_days = 14;
  REQUIRE(save_settings(source));

  const Settings loaded = load_settings();
  CHECK_FALSE(loaded.recovery_enabled);
  CHECK_FALSE(loaded.restore_session);
  CHECK(loaded.recovery_max_bytes == (32ull << 20));
  CHECK(loaded.recovery_retention_days == 14);
}
