#include "listopad/session.h"

#include "listopad/strings.h"

#include <windows.h>
#include <yyjson.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <span>
#include <system_error>
#include <utility>

namespace listopad {
namespace {

std::span<const std::byte> bytes(const std::string_view value) {
  return std::as_bytes(std::span(value.data(), value.size()));
}

std::string eol_name(const EolMode mode) {
  switch (mode) {
    case EolMode::Lf:
      return "lf";
    case EolMode::Cr:
      return "cr";
    case EolMode::Mixed:
      return "mixed";
    default:
      return "crlf";
  }
}

EolMode parse_eol(const std::string_view value) {
  if (value == "lf") return EolMode::Lf;
  if (value == "cr") return EolMode::Cr;
  if (value == "mixed") return EolMode::Mixed;
  return EolMode::CrLf;
}

std::string view_name(const StoredViewKind view) {
  switch (view) {
    case StoredViewKind::LargeText:
      return "large-text";
    case StoredViewKind::Hex:
      return "hex";
    default:
      return "text";
  }
}

StoredViewKind parse_view(const std::string_view value) {
  if (value == "large-text") return StoredViewKind::LargeText;
  if (value == "hex") return StoredViewKind::Hex;
  return StoredViewKind::Text;
}

std::string string_value(yyjson_val* object, const char* key,
                         const std::string_view fallback = {}) {
  yyjson_val* value = yyjson_obj_get(object, key);
  return yyjson_is_str(value) ? std::string(yyjson_get_str(value))
                              : std::string(fallback);
}

std::int64_t integer_value(yyjson_val* object, const char* key,
                           const std::int64_t fallback = 0) {
  yyjson_val* value = yyjson_obj_get(object, key);
  return yyjson_is_int(value) ? yyjson_get_sint(value) : fallback;
}

std::uint64_t unsigned_value(yyjson_val* object, const char* key,
                             const std::uint64_t fallback = 0) {
  yyjson_val* value = yyjson_obj_get(object, key);
  return yyjson_is_uint(value) ? yyjson_get_uint(value) : fallback;
}

bool bool_value(yyjson_val* object, const char* key,
                const bool fallback = false) {
  yyjson_val* value = yyjson_obj_get(object, key);
  return yyjson_is_bool(value) ? yyjson_get_bool(value) : fallback;
}

std::filesystem::path path_value(yyjson_val* object, const char* key) {
  return std::filesystem::path(
      utf8_to_wide(string_value(object, key)));
}

void add_string(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key,
                const std::string_view value) {
  yyjson_mut_obj_add_strncpy(doc, object, key, value.data(), value.size());
}

void add_path(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key,
              const std::filesystem::path& value) {
  add_string(doc, object, key, wide_to_utf8(value.wstring()));
}

void add_wstring(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key,
                 const std::wstring_view value) {
  add_string(doc, object, key, wide_to_utf8(value));
}

yyjson_mut_val* write_session_tab(yyjson_mut_doc* doc,
                                  const SessionTab& tab) {
  yyjson_mut_val* object = yyjson_mut_obj(doc);
  add_string(doc, object, "id", tab.id);
  add_path(doc, object, "path", tab.path);
  add_wstring(doc, object, "title", tab.title);
  add_string(doc, object, "encoding", tab.encoding.name());
  add_string(doc, object, "eol", eol_name(tab.eol));
  add_string(doc, object, "language", tab.language);
  add_string(doc, object, "view", view_name(tab.view));
  yyjson_mut_obj_add_sint(doc, object, "caret", tab.caret);
  yyjson_mut_obj_add_sint(doc, object, "anchor", tab.anchor);
  yyjson_mut_obj_add_sint(doc, object, "firstVisibleLine",
                          tab.first_visible_line);
  yyjson_mut_obj_add_sint(doc, object, "xOffset", tab.x_offset);
  yyjson_mut_obj_add_bool(doc, object, "externalDiverged",
                          tab.external_diverged);
  return object;
}

bool read_session_tab(yyjson_val* object, SessionTab& tab) {
  if (!yyjson_is_obj(object)) return false;
  tab.id = string_value(object, "id");
  if (!valid_session_id(tab.id)) return false;
  tab.path = path_value(object, "path");
  tab.title = utf8_to_wide(string_value(object, "title"));
  tab.encoding =
      encoding_from_name(string_value(object, "encoding", "utf-8"));
  tab.eol = parse_eol(string_value(object, "eol", "crlf"));
  tab.language = string_value(object, "language", "text");
  tab.view = parse_view(string_value(object, "view", "text"));
  tab.caret = integer_value(object, "caret");
  tab.anchor = integer_value(object, "anchor");
  tab.first_visible_line = integer_value(object, "firstVisibleLine");
  tab.x_offset = integer_value(object, "xOffset");
  tab.external_diverged = bool_value(object, "externalDiverged");
  if (tab.title.empty()) {
    tab.title = tab.path.empty() ? L"Untitled" : tab.path.filename().wstring();
  }
  return true;
}

void add_fingerprint(yyjson_mut_doc* doc, yyjson_mut_val* object,
                     const FileFingerprint& fingerprint) {
  yyjson_mut_val* value = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_bool(doc, value, "exists", fingerprint.exists);
  yyjson_mut_obj_add_uint(doc, value, "volumeSerial",
                          fingerprint.volume_serial);
  add_string(doc, value, "fileId", hex_encode(fingerprint.file_id));
  yyjson_mut_obj_add_uint(doc, value, "size", fingerprint.size);
  yyjson_mut_obj_add_uint(doc, value, "lastWrite", fingerprint.last_write);
  yyjson_mut_obj_add_val(doc, object, "sourceFingerprint", value);
}

int hex_digit(const char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

void read_fingerprint(yyjson_val* object, FileFingerprint& fingerprint) {
  yyjson_val* value = yyjson_obj_get(object, "sourceFingerprint");
  if (!yyjson_is_obj(value)) return;
  fingerprint.exists = bool_value(value, "exists");
  fingerprint.volume_serial = unsigned_value(value, "volumeSerial");
  fingerprint.size = unsigned_value(value, "size");
  fingerprint.last_write = unsigned_value(value, "lastWrite");
  const std::string file_id = string_value(value, "fileId");
  if (file_id.size() != fingerprint.file_id.size() * 2) return;
  for (std::size_t index = 0; index < fingerprint.file_id.size(); ++index) {
    const int high = hex_digit(file_id[index * 2]);
    const int low = hex_digit(file_id[index * 2 + 1]);
    if (high < 0 || low < 0) {
      fingerprint.file_id = {};
      return;
    }
    fingerprint.file_id[index] =
        static_cast<std::byte>((high << 4) | low);
  }
}

bool write_json(const std::filesystem::path& path, yyjson_mut_doc* doc) {
  std::size_t length = 0;
  char* json = yyjson_mut_write(
      doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length);
  if (!json) return false;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  const SaveFileResult saved =
      atomic_save(path, std::span(reinterpret_cast<const std::byte*>(json),
                                  length),
                  std::nullopt, true);
  std::free(json);
  return saved.status == SaveStatus::Saved;
}

yyjson_doc* read_json(const std::filesystem::path& path,
                      StoreLoadStatus& status) {
  const ReadFileResult file = read_file(path);
  if (!file.ok) {
    status = file.error == ERROR_FILE_NOT_FOUND ||
                     file.error == ERROR_PATH_NOT_FOUND
                 ? StoreLoadStatus::Missing
                 : StoreLoadStatus::Invalid;
    return nullptr;
  }
  yyjson_read_err error{};
  yyjson_doc* doc = yyjson_read_opts(
      reinterpret_cast<char*>(const_cast<std::byte*>(file.bytes.data())),
      file.bytes.size(), 0, nullptr, &error);
  if (!doc) status = StoreLoadStatus::Invalid;
  return doc;
}

void add_path_array(yyjson_mut_doc* doc, yyjson_mut_val* root,
                    const char* key,
                    const std::vector<std::filesystem::path>& paths) {
  yyjson_mut_val* array = yyjson_mut_arr(doc);
  for (const auto& path : paths) {
    const std::string utf8 = wide_to_utf8(path.wstring());
    yyjson_mut_arr_add_strncpy(doc, array, utf8.data(), utf8.size());
  }
  yyjson_mut_obj_add_val(doc, root, key, array);
}

void read_path_array(yyjson_val* root, const char* key,
                     std::vector<std::filesystem::path>& paths) {
  yyjson_val* array = yyjson_obj_get(root, key);
  if (!yyjson_is_arr(array)) return;
  std::size_t index = 0;
  std::size_t maximum = 0;
  yyjson_val* value = nullptr;
  yyjson_arr_foreach(array, index, maximum, value) {
    if (yyjson_is_str(value)) {
      paths.emplace_back(utf8_to_wide(yyjson_get_str(value)));
    }
  }
}

}  // namespace

SessionStore::SessionStore(std::filesystem::path profile_directory)
    : profile_directory_(std::move(profile_directory)) {}

std::filesystem::path SessionStore::manifest_path() const {
  return profile_directory_ / L"session.json";
}

std::filesystem::path SessionStore::recovery_directory() const {
  return profile_directory_ / L"recovery";
}

std::filesystem::path SessionStore::recovery_path(
    const std::string& id) const {
  if (!valid_session_id(id)) return {};
  return recovery_directory() / (utf8_to_wide(id) + L".json");
}

StoreLoadResult<SessionManifest> SessionStore::load_manifest() const {
  StoreLoadResult<SessionManifest> result;
  yyjson_doc* doc = read_json(manifest_path(), result.status);
  if (!doc) return result;
  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    result.status = StoreLoadStatus::Invalid;
    yyjson_doc_free(doc);
    return result;
  }
  const auto schema = static_cast<int>(integer_value(root, "schemaVersion"));
  if (schema != kSessionSchemaVersion) {
    result.status = StoreLoadStatus::UnsupportedVersion;
    yyjson_doc_free(doc);
    return result;
  }
  result.value.schema_version = schema;
  result.value.clean_shutdown = bool_value(root, "cleanShutdown");
  result.value.active_index =
      static_cast<int>(integer_value(root, "activeIndex", -1));
  yyjson_val* tabs = yyjson_obj_get(root, "tabs");
  if (yyjson_is_arr(tabs)) {
    std::size_t index = 0;
    std::size_t maximum = 0;
    yyjson_val* value = nullptr;
    yyjson_arr_foreach(tabs, index, maximum, value) {
      SessionTab tab;
      if (read_session_tab(value, tab)) {
        result.value.tabs.push_back(std::move(tab));
      }
    }
  }
  read_path_array(root, "recentFiles", result.value.recent_files);
  read_path_array(root, "closedFiles", result.value.closed_files);
  result.status = StoreLoadStatus::Loaded;
  yyjson_doc_free(doc);
  return result;
}

bool SessionStore::save_manifest(const SessionManifest& manifest) const {
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  if (!doc) return false;
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_int(doc, root, "schemaVersion",
                         kSessionSchemaVersion);
  yyjson_mut_obj_add_bool(doc, root, "cleanShutdown",
                          manifest.clean_shutdown);
  yyjson_mut_obj_add_int(doc, root, "activeIndex", manifest.active_index);
  yyjson_mut_val* tabs = yyjson_mut_arr(doc);
  for (const auto& tab : manifest.tabs) {
    yyjson_mut_arr_add_val(tabs, write_session_tab(doc, tab));
  }
  yyjson_mut_obj_add_val(doc, root, "tabs", tabs);
  add_path_array(doc, root, "recentFiles", manifest.recent_files);
  add_path_array(doc, root, "closedFiles", manifest.closed_files);
  const bool saved = write_json(manifest_path(), doc);
  yyjson_mut_doc_free(doc);
  return saved;
}

bool SessionStore::clear_manifest() const {
  std::error_code error;
  const bool removed = std::filesystem::remove(manifest_path(), error);
  return !error &&
         (removed || !std::filesystem::exists(manifest_path(), error));
}

StoreLoadResult<RecoverySnapshot> SessionStore::load_recovery(
    const std::filesystem::path& path) const {
  StoreLoadResult<RecoverySnapshot> result;
  yyjson_doc* doc = read_json(path, result.status);
  if (!doc) return result;
  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    result.status = StoreLoadStatus::Invalid;
    yyjson_doc_free(doc);
    return result;
  }
  const auto schema = static_cast<int>(integer_value(root, "schemaVersion"));
  if (schema != kSessionSchemaVersion) {
    result.status = StoreLoadStatus::UnsupportedVersion;
    yyjson_doc_free(doc);
    return result;
  }
  RecoverySnapshot& snapshot = result.value;
  snapshot.schema_version = schema;
  snapshot.id = string_value(root, "id");
  snapshot.path = path_value(root, "path");
  snapshot.title = utf8_to_wide(string_value(root, "title"));
  snapshot.encoding =
      encoding_from_name(string_value(root, "encoding", "utf-8"));
  snapshot.eol = parse_eol(string_value(root, "eol", "crlf"));
  snapshot.language = string_value(root, "language", "text");
  snapshot.content = string_value(root, "content");
  snapshot.created_unix_ms = unsigned_value(root, "createdUnixMs");
  snapshot.content_sha256 = string_value(root, "contentSha256");
  snapshot.external_diverged = bool_value(root, "externalDiverged");
  read_fingerprint(root, snapshot.source_fingerprint);
  if (!valid_session_id(snapshot.id) ||
      snapshot.content_sha256 !=
          hex_encode(sha256(bytes(snapshot.content)))) {
    result.status = StoreLoadStatus::Invalid;
  } else {
    result.status = StoreLoadStatus::Loaded;
  }
  yyjson_doc_free(doc);
  return result;
}

std::vector<RecoverySnapshot> SessionStore::load_recovery_snapshots() const {
  std::vector<RecoverySnapshot> result;
  std::error_code error;
  if (!std::filesystem::exists(recovery_directory(), error)) return result;
  for (const auto& entry :
       std::filesystem::directory_iterator(recovery_directory(), error)) {
    if (error) break;
    if (!entry.is_regular_file(error) || entry.path().extension() != L".json") {
      continue;
    }
    auto loaded = load_recovery(entry.path());
    if (loaded.status == StoreLoadStatus::Loaded) {
      result.push_back(std::move(loaded.value));
    }
  }
  std::sort(result.begin(), result.end(),
            [](const RecoverySnapshot& left,
               const RecoverySnapshot& right) {
              return left.created_unix_ms < right.created_unix_ms;
            });
  return result;
}

bool SessionStore::save_recovery(const RecoverySnapshot& input) const {
  if (!valid_session_id(input.id)) return false;
  RecoverySnapshot snapshot = input;
  snapshot.schema_version = kSessionSchemaVersion;
  snapshot.content_sha256 =
      hex_encode(sha256(bytes(snapshot.content)));
  if (snapshot.created_unix_ms == 0) {
    snapshot.created_unix_ms = unix_time_milliseconds();
  }

  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  if (!doc) return false;
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_int(doc, root, "schemaVersion",
                         kSessionSchemaVersion);
  add_string(doc, root, "id", snapshot.id);
  add_path(doc, root, "path", snapshot.path);
  add_wstring(doc, root, "title", snapshot.title);
  add_string(doc, root, "encoding", snapshot.encoding.name());
  add_string(doc, root, "eol", eol_name(snapshot.eol));
  add_string(doc, root, "language", snapshot.language);
  add_fingerprint(doc, root, snapshot.source_fingerprint);
  add_string(doc, root, "content", snapshot.content);
  yyjson_mut_obj_add_uint(doc, root, "createdUnixMs",
                          snapshot.created_unix_ms);
  add_string(doc, root, "contentSha256", snapshot.content_sha256);
  yyjson_mut_obj_add_bool(doc, root, "externalDiverged",
                          snapshot.external_diverged);
  const bool saved = write_json(recovery_path(snapshot.id), doc);
  yyjson_mut_doc_free(doc);
  return saved;
}

bool SessionStore::remove_recovery(const std::string& id) const {
  const auto path = recovery_path(id);
  if (path.empty()) return false;
  std::error_code error;
  const bool removed = std::filesystem::remove(path, error);
  return !error && (removed || !std::filesystem::exists(path, error));
}

bool SessionStore::clear_recovery() const {
  std::error_code error;
  if (!std::filesystem::exists(recovery_directory(), error)) return true;
  std::filesystem::remove_all(recovery_directory(), error);
  return !error;
}

std::size_t SessionStore::remove_expired_recovery(
    const std::uint64_t oldest_allowed_unix_ms) const {
  std::size_t removed = 0;
  std::error_code error;
  if (!std::filesystem::exists(recovery_directory(), error)) return 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(recovery_directory(), error)) {
    if (error) break;
    if (!entry.is_regular_file(error) || entry.path().extension() != L".json") {
      continue;
    }
    const auto loaded = load_recovery(entry.path());
    if (loaded.status == StoreLoadStatus::Loaded &&
        loaded.value.created_unix_ms < oldest_allowed_unix_ms &&
        std::filesystem::remove(entry.path(), error)) {
      ++removed;
    }
    error.clear();
  }
  return removed;
}

std::uint64_t unix_time_milliseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

bool valid_session_id(const std::string_view id) noexcept {
  if (id.size() < 8 || id.size() > 64) return false;
  return std::all_of(id.begin(), id.end(), [](const char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F') || value == '-';
  });
}

}  // namespace listopad
