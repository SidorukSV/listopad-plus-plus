#include "listopad/shell_registration.h"

#include "listopad/strings.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <string>

namespace listopad {
namespace {
constexpr wchar_t kKey[] = L"Software\\Classes\\*\\shell\\ListopadPP";

std::filesystem::path executable_path() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length); return path;
}
bool set_text(HKEY key, const wchar_t* name, const std::wstring& value) {
  return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
}  // namespace

bool register_classic_context_menu(const std::string_view ui_language) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
  const auto exe = executable_path();
  const std::wstring title = ui_language == "en" ? L"Open in Listopad++" : L"Открыть в Listopad++";
  bool ok = set_text(key, L"MUIVerb", title) && set_text(key, L"Icon", exe.wstring());
  HKEY command = nullptr;
  if (ok && RegCreateKeyExW(key, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &command, nullptr) == ERROR_SUCCESS) {
    ok = set_text(command, nullptr, quote_command_line_argument(exe.wstring()) + L" \"%1\"");
    RegCloseKey(command);
  } else ok = false;
  RegCloseKey(key); SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); return ok;
}

void record_executable_location() {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ListopadPP", 0, nullptr, 0, KEY_WRITE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  set_text(key, L"ExecutablePath", executable_path().wstring());
  RegCloseKey(key);
}

bool unregister_classic_context_menu() {
  const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, kKey);
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}
}  // namespace listopad
