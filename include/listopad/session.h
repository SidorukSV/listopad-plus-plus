#pragma once

#include "listopad/encoding.h"
#include "listopad/file_io.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace listopad {

inline constexpr int kSessionSchemaVersion = 1;

enum class StoredViewKind {
  Text,
  LargeText,
  Hex,
};

struct SessionTab {
  std::string id;
  std::filesystem::path path;
  std::wstring title;
  Encoding encoding;
  EolMode eol{EolMode::CrLf};
  std::string language{"text"};
  StoredViewKind view{StoredViewKind::Text};
  std::int64_t caret{0};
  std::int64_t anchor{0};
  std::int64_t first_visible_line{0};
  std::int64_t x_offset{0};
  bool external_diverged{false};
};

struct SessionManifest {
  int schema_version{kSessionSchemaVersion};
  bool clean_shutdown{false};
  int active_index{-1};
  std::vector<SessionTab> tabs;
  std::vector<std::filesystem::path> recent_files;
  std::vector<std::filesystem::path> closed_files;
};

struct RecoverySnapshot {
  int schema_version{kSessionSchemaVersion};
  std::string id;
  std::filesystem::path path;
  std::wstring title;
  Encoding encoding;
  EolMode eol{EolMode::CrLf};
  std::string language{"text"};
  FileFingerprint source_fingerprint;
  std::string content;
  std::uint64_t created_unix_ms{0};
  std::string content_sha256;
  bool external_diverged{false};
};

enum class StoreLoadStatus {
  Missing,
  Loaded,
  Invalid,
  UnsupportedVersion,
};

template <typename Value>
struct StoreLoadResult {
  StoreLoadStatus status{StoreLoadStatus::Missing};
  Value value;
};

class SessionStore final {
 public:
  explicit SessionStore(std::filesystem::path profile_directory);

  [[nodiscard]] const std::filesystem::path& profile_directory() const noexcept {
    return profile_directory_;
  }
  [[nodiscard]] std::filesystem::path manifest_path() const;
  [[nodiscard]] std::filesystem::path recovery_directory() const;
  [[nodiscard]] std::filesystem::path recovery_path(
      const std::string& id) const;

  [[nodiscard]] StoreLoadResult<SessionManifest> load_manifest() const;
  [[nodiscard]] bool save_manifest(const SessionManifest& manifest) const;
  [[nodiscard]] bool clear_manifest() const;

  [[nodiscard]] StoreLoadResult<RecoverySnapshot> load_recovery(
      const std::filesystem::path& path) const;
  [[nodiscard]] std::vector<RecoverySnapshot> load_recovery_snapshots() const;
  [[nodiscard]] bool save_recovery(const RecoverySnapshot& snapshot) const;
  [[nodiscard]] bool remove_recovery(const std::string& id) const;
  [[nodiscard]] bool clear_recovery() const;
  [[nodiscard]] std::size_t remove_expired_recovery(
      std::uint64_t oldest_allowed_unix_ms) const;

 private:
  std::filesystem::path profile_directory_;
};

[[nodiscard]] std::uint64_t unix_time_milliseconds();
[[nodiscard]] bool valid_session_id(std::string_view id) noexcept;

}  // namespace listopad
