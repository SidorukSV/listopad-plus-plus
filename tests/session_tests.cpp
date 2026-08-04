#include "listopad/session.h"

#include "listopad/file_io.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <filesystem>
#include <string>

using namespace listopad;

namespace {

class TestDirectory final {
 public:
  TestDirectory() {
    std::wstring root(32768, L'\0');
    const DWORD length =
        GetTempPathW(static_cast<DWORD>(root.size()), root.data());
    root.resize(length);
    path_ = std::filesystem::path(root) /
            (L"ListopadPP.session-tests." +
             std::to_wstring(GetCurrentProcessId()) + L"." +
             std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

std::span<const std::byte> as_bytes(const std::string_view value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}

}  // namespace

TEST_CASE("session manifest round trips Unicode paths and view state") {
  const TestDirectory directory;
  const SessionStore store(directory.path());

  SessionManifest source;
  source.clean_shutdown = true;
  source.active_index = 0;
  SessionTab tab;
  tab.id = "0123456789abcdef";
  tab.path = L"C:\\данные\\конфиг.json";
  tab.title = L"конфиг.json";
  tab.encoding = {EncodingKind::WindowsCodePage, 1251, false};
  tab.eol = EolMode::Lf;
  tab.language = "json";
  tab.view = StoredViewKind::Hex;
  tab.caret = 41;
  tab.anchor = 17;
  tab.first_visible_line = 9;
  tab.x_offset = 12;
  tab.external_diverged = true;
  source.tabs.push_back(tab);
  source.recent_files = {L"C:\\данные\\конфиг.json"};
  source.closed_files = {L"\\\\server\\share\\закрыт.txt"};

  REQUIRE(store.save_manifest(source));
  const auto loaded = store.load_manifest();
  REQUIRE(loaded.status == StoreLoadStatus::Loaded);
  REQUIRE(loaded.value.tabs.size() == 1);
  const SessionTab& restored = loaded.value.tabs.front();
  CHECK(restored.id == tab.id);
  CHECK(restored.path == tab.path);
  CHECK(restored.title == tab.title);
  CHECK(restored.encoding == tab.encoding);
  CHECK(restored.eol == tab.eol);
  CHECK(restored.language == tab.language);
  CHECK(restored.view == tab.view);
  CHECK(restored.caret == tab.caret);
  CHECK(restored.anchor == tab.anchor);
  CHECK(restored.first_visible_line == tab.first_visible_line);
  CHECK(restored.x_offset == tab.x_offset);
  CHECK(restored.external_diverged);
  CHECK(loaded.value.recent_files == source.recent_files);
  CHECK(loaded.value.closed_files == source.closed_files);
  REQUIRE(store.clear_manifest());
  CHECK(store.load_manifest().status == StoreLoadStatus::Missing);
}

TEST_CASE("recovery snapshot validates content and source fingerprint") {
  const TestDirectory directory;
  const SessionStore store(directory.path());

  RecoverySnapshot source;
  source.id = "fedcba9876543210";
  source.path = L"C:\\данные\\черновик.bsl";
  source.title = L"черновик.bsl";
  source.encoding = {EncodingKind::Utf8, CP_UTF8, true};
  source.eol = EolMode::CrLf;
  source.language = "bsl";
  source.source_fingerprint.exists = true;
  source.source_fingerprint.volume_serial = 42;
  source.source_fingerprint.size = 100;
  source.source_fingerprint.last_write = 500;
  source.source_fingerprint.file_id[0] = std::byte{0xab};
  source.content = "Процедура Тест()\r\nКонецПроцедуры\r\n";
  source.created_unix_ms = 123456789;
  source.external_diverged = true;

  REQUIRE(store.save_recovery(source));
  const auto loaded = store.load_recovery(store.recovery_path(source.id));
  REQUIRE(loaded.status == StoreLoadStatus::Loaded);
  CHECK(loaded.value.path == source.path);
  CHECK(loaded.value.title == source.title);
  CHECK(loaded.value.encoding == source.encoding);
  CHECK(loaded.value.content == source.content);
  CHECK(loaded.value.source_fingerprint == source.source_fingerprint);
  CHECK(loaded.value.external_diverged);

  const std::string corrupt =
      "{\"schemaVersion\":1,\"id\":\"fedcba9876543210\","
      "\"content\":\"changed\",\"contentSha256\":\"bad\"}";
  REQUIRE(atomic_save(store.recovery_path(source.id), as_bytes(corrupt),
                      std::nullopt, true)
              .status == SaveStatus::Saved);
  CHECK(store.load_recovery(store.recovery_path(source.id)).status ==
        StoreLoadStatus::Invalid);
}

TEST_CASE("unknown session schema is preserved and rejected") {
  const TestDirectory directory;
  const SessionStore store(directory.path());
  const std::string future =
      "{\"schemaVersion\":99,\"cleanShutdown\":true,\"tabs\":[]}";
  REQUIRE(atomic_save(store.manifest_path(), as_bytes(future), std::nullopt,
                      true)
              .status == SaveStatus::Saved);

  CHECK(store.load_manifest().status ==
        StoreLoadStatus::UnsupportedVersion);
  CHECK(std::filesystem::exists(store.manifest_path()));
}

TEST_CASE("expired recovery cleanup keeps recent snapshots") {
  const TestDirectory directory;
  const SessionStore store(directory.path());

  RecoverySnapshot old;
  old.id = "aaaaaaaaaaaaaaaa";
  old.title = L"old";
  old.content = "old";
  old.created_unix_ms = 10;
  RecoverySnapshot recent = old;
  recent.id = "bbbbbbbbbbbbbbbb";
  recent.title = L"recent";
  recent.created_unix_ms = 30;
  REQUIRE(store.save_recovery(old));
  REQUIRE(store.save_recovery(recent));

  CHECK(store.remove_expired_recovery(20) == 1);
  const auto snapshots = store.load_recovery_snapshots();
  REQUIRE(snapshots.size() == 1);
  CHECK(snapshots.front().id == recent.id);
}
