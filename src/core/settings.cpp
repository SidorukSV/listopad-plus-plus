#include "listopad/settings.h"

#include "listopad/file_io.h"

#include <windows.h>
#include <shlobj.h>
#include <yyjson.h>

#include <cstdlib>
#include <algorithm>

namespace listopad {
namespace {

std::filesystem::path executable_directory() {
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
}

void read_string(yyjson_val* root, const char* key, std::string& target) {
  yyjson_val* value = yyjson_obj_get(root, key);
  if (yyjson_is_str(value)) target = yyjson_get_str(value);
}

}  // namespace

bool portable_mode() { return std::filesystem::exists(executable_directory() / L"portable.flag"); }

std::filesystem::path settings_path() {
  if (portable_mode()) return executable_directory() / L"settings.json";
  PWSTR local = nullptr;
  std::filesystem::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) {
    result = std::filesystem::path(local) / L"Listopad++" / L"settings.json";
    CoTaskMemFree(local);
  } else {
    result = executable_directory() / L"settings.json";
  }
  return result;
}

Settings load_settings() {
  Settings settings;
  const ReadFileResult file = read_file(settings_path());
  if (!file.ok || file.bytes.empty()) return settings;
  yyjson_read_err error{};
  yyjson_doc* doc = yyjson_read_opts(
      reinterpret_cast<char*>(const_cast<std::byte*>(file.bytes.data())),
      file.bytes.size(), 0, nullptr, &error);
  if (!doc) return settings;
  yyjson_val* root = yyjson_doc_get_root(doc);
  if (yyjson_is_obj(root)) {
    read_string(root, "uiLanguage", settings.ui_language);
    read_string(root, "theme", settings.theme);
    read_string(root, "fontFace", settings.font_face);
    read_string(root, "fallbackEncoding", settings.fallback_encoding);
    if (yyjson_val* value = yyjson_obj_get(root, "fontSize"); yyjson_is_int(value)) settings.font_size = static_cast<int>(yyjson_get_int(value));
    if (yyjson_val* value = yyjson_obj_get(root, "indentSize"); yyjson_is_int(value)) settings.indent_size = static_cast<int>(yyjson_get_int(value));
    if (yyjson_val* value = yyjson_obj_get(root, "indentWithTabs"); yyjson_is_bool(value)) settings.indent_with_tabs = yyjson_get_bool(value);
    if (yyjson_val* value = yyjson_obj_get(root, "documentMap"); yyjson_is_bool(value)) settings.show_document_map = yyjson_get_bool(value);
    if (yyjson_val* value = yyjson_obj_get(root, "largeFileThreshold"); yyjson_is_uint(value)) settings.large_file_threshold = yyjson_get_uint(value);
    if (yyjson_val* window = yyjson_obj_get(root, "window"); yyjson_is_obj(window)) {
      const auto read_int = [](yyjson_val* obj, const char* key, int& target) {
        if (yyjson_val* value = yyjson_obj_get(obj, key); yyjson_is_int(value)) target = static_cast<int>(yyjson_get_int(value));
      };
      read_int(window, "x", settings.window.x);
      read_int(window, "y", settings.window.y);
      read_int(window, "width", settings.window.width);
      read_int(window, "height", settings.window.height);
      if (yyjson_val* value = yyjson_obj_get(window, "maximized"); yyjson_is_bool(value)) settings.window.maximized = yyjson_get_bool(value);
      // A stored rectangle is only meaningful with a positive extent; anything
      // else is treated as "no saved geometry" so the app falls back to defaults.
      settings.window.valid = settings.window.width > 0 && settings.window.height > 0;
    }
  }
  yyjson_doc_free(doc);
  settings.font_size = std::clamp(settings.font_size, 7, 40);
  settings.indent_size = std::clamp(settings.indent_size, 1, 8);
  settings.large_file_threshold = std::clamp<std::uint64_t>(settings.large_file_threshold, 16ull << 20, 4ull << 30);
  return settings;
}

bool save_settings(const Settings& settings) {
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "uiLanguage", settings.ui_language.c_str());
  yyjson_mut_obj_add_str(doc, root, "theme", settings.theme.c_str());
  yyjson_mut_obj_add_str(doc, root, "fontFace", settings.font_face.c_str());
  yyjson_mut_obj_add_int(doc, root, "fontSize", settings.font_size);
  yyjson_mut_obj_add_int(doc, root, "indentSize", settings.indent_size);
  yyjson_mut_obj_add_bool(doc, root, "indentWithTabs", settings.indent_with_tabs);
  yyjson_mut_obj_add_bool(doc, root, "documentMap", settings.show_document_map);
  yyjson_mut_obj_add_uint(doc, root, "largeFileThreshold", settings.large_file_threshold);
  yyjson_mut_obj_add_str(doc, root, "fallbackEncoding", settings.fallback_encoding.c_str());
  if (settings.window.valid) {
    yyjson_mut_val* window = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, window, "x", settings.window.x);
    yyjson_mut_obj_add_int(doc, window, "y", settings.window.y);
    yyjson_mut_obj_add_int(doc, window, "width", settings.window.width);
    yyjson_mut_obj_add_int(doc, window, "height", settings.window.height);
    yyjson_mut_obj_add_bool(doc, window, "maximized", settings.window.maximized);
    yyjson_mut_obj_add_val(doc, root, "window", window);
  }
  std::size_t length = 0;
  char* json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END, &length);
  bool ok = false;
  if (json) {
    std::error_code error;
    std::filesystem::create_directories(settings_path().parent_path(), error);
    const auto bytes = std::span(reinterpret_cast<const std::byte*>(json), length);
    ok = atomic_save(settings_path(), bytes, std::nullopt, true).status == SaveStatus::Saved;
    std::free(json);
  }
  yyjson_mut_doc_free(doc);
  return ok;
}

}  // namespace listopad
