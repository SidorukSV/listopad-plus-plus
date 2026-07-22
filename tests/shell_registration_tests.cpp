#include "listopad/shell_registration.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <string>

namespace {
// Never the real key: overwriting it would point the installed shell extension
// at the test binary and break the context menu on the developer's machine.
constexpr wchar_t kTestKey[] = L"Software\\ListopadPP\\TestScratch";

struct ScratchKey {
  ~ScratchKey() { RegDeleteTreeW(HKEY_CURRENT_USER, kTestKey); }
};

std::filesystem::path current_executable() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length);
  return path;
}

void write_value(const std::wstring& value) {
  HKEY key = nullptr;
  REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, kTestKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                          nullptr) == ERROR_SUCCESS);
  REQUIRE(RegSetValueExW(key, L"ExecutablePath", 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(value.c_str()),
                         static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
  RegCloseKey(key);
}
}  // namespace

TEST_CASE("recorded executable location round-trips through the registry") {
  ScratchKey cleanup;
  listopad::record_executable_location(kTestKey);
  const auto recorded = listopad::recorded_executable_location(kTestKey);
  REQUIRE(recorded.has_value());
  CHECK(std::filesystem::equivalent(*recorded, current_executable()));
}

TEST_CASE("a missing value falls back instead of returning a path") {
  RegDeleteTreeW(HKEY_CURRENT_USER, kTestKey);
  CHECK_FALSE(listopad::recorded_executable_location(kTestKey).has_value());
}

TEST_CASE("an empty value is treated as absent") {
  ScratchKey cleanup;
  write_value(L"");
  CHECK_FALSE(listopad::recorded_executable_location(kTestKey).has_value());
}

TEST_CASE("a path that no longer exists is treated as absent") {
  ScratchKey cleanup;
  // A stale value must not be handed to CreateProcess: the caller's fallback
  // has a chance of being right, a deleted path never is.
  write_value((std::filesystem::temp_directory_path() / L"listopad-does-not-exist.exe").wstring());
  CHECK_FALSE(listopad::recorded_executable_location(kTestKey).has_value());
}

TEST_CASE("recording twice keeps the latest value readable") {
  ScratchKey cleanup;
  write_value(L"");
  listopad::record_executable_location(kTestKey);
  const auto recorded = listopad::recorded_executable_location(kTestKey);
  REQUIRE(recorded.has_value());
  CHECK(std::filesystem::equivalent(*recorded, current_executable()));
}
