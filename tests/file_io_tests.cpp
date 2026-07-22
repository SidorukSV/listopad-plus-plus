#include "listopad/file_io.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <string>

using namespace listopad;

namespace {

std::filesystem::path unique_test_directory() {
  std::wstring root(32768, L'\0');
  const DWORD length = GetTempPathW(static_cast<DWORD>(root.size()), root.data());
  root.resize(length);
  const auto path = std::filesystem::path(root) /
      (L"ListopadPP.tests." + std::to_wstring(GetCurrentProcessId()) + L"." +
       std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(path);
  return path;
}

std::span<const std::byte> bytes(const std::string& value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}

}  // namespace

TEST_CASE("atomic save refuses to overwrite a newer disk fingerprint") {
  const auto directory = unique_test_directory();
  const auto path = directory / L"conflict.txt";
  const std::string first = "first";
  REQUIRE(atomic_save(path, bytes(first), std::nullopt, false).status == SaveStatus::Saved);
  const FileFingerprint expected = fingerprint_file(path);

  const std::string external = "external change";
  REQUIRE(atomic_save(path, bytes(external), std::nullopt, true).status == SaveStatus::Saved);
  const std::string local = "local change";
  REQUIRE(atomic_save(path, bytes(local), expected, false).status == SaveStatus::Conflict);

  const ReadFileResult read = read_file(path);
  REQUIRE(read.ok);
  REQUIRE(std::string(reinterpret_cast<const char*>(read.bytes.data()), read.bytes.size()) == external);
  std::filesystem::remove_all(directory);
}

TEST_CASE("SHA-256 implementation returns the known empty digest") {
  REQUIRE(hex_encode(sha256({})) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
