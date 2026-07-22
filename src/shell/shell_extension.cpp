#include "listopad/shell_registration.h"
#include "listopad/strings.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <atomic>
#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>

namespace {

// {D7ED11B1-F908-49D8-974E-C98670A90A13}
constexpr CLSID kListopadCommand{0xd7ed11b1, 0xf908, 0x49d8,
                                 {0x97, 0x4e, 0xc9, 0x86, 0x70, 0xa9, 0x0a, 0x13}};
std::atomic_ulong g_objects{0};
HINSTANCE g_instance{};

std::filesystem::path module_directory() {
  std::wstring path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(g_instance, path.data(), static_cast<DWORD>(path.size()));
  path.resize(length); return std::filesystem::path(path).parent_path();
}

// The packaged layout keeps this DLL in the package's external content
// directory while the editor ships elsewhere, so it publishes its own location
// on startup. Fall back to the historical layout for portable, classic and MSI
// installs, where the two share a directory and the value may never have been
// written.
std::filesystem::path editor_path() {
  if (const auto recorded = listopad::recorded_executable_location()) return *recorded;
  return module_directory() / L"ListopadPP.exe";
}

std::filesystem::path editor_directory() { return editor_path().parent_path(); }

std::filesystem::path shell_settings_path() {
  const auto directory = editor_directory();
  if (std::filesystem::exists(directory / L"portable.flag")) return directory / L"settings.json";
  PWSTR local = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local))) {
    const std::filesystem::path path = std::filesystem::path(local) / L"Listopad++" / L"settings.json";
    CoTaskMemFree(local);
    return path;
  }
  return directory / L"settings.json";
}

std::optional<bool> russian_from_settings() {
  const auto path = shell_settings_path();
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024) {
    CloseHandle(file);
    return std::nullopt;
  }
  std::string json(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool ok = ReadFile(file, json.data(), static_cast<DWORD>(json.size()), &read, nullptr) != FALSE;
  CloseHandle(file);
  if (!ok) return std::nullopt;
  json.resize(read);

  constexpr std::string_view key = "\"uiLanguage\"";
  std::size_t value = json.find(key);
  if (value == std::string::npos || (value = json.find(':', value + key.size())) == std::string::npos)
    return std::nullopt;
  value = json.find_first_not_of(" \t\r\n", value + 1);
  if (value == std::string::npos) return std::nullopt;
  if (json.compare(value, 4, "\"en\"") == 0) return false;
  if (json.compare(value, 4, "\"ru\"") == 0) return true;
  return std::nullopt;
}

bool russian_ui() {
  static const bool russian = russian_from_settings().value_or(true);
  return russian;
}

class ExplorerCommand final : public IExplorerCommand {
 public:
  ExplorerCommand() { ++g_objects; }
  ~ExplorerCommand() { --g_objects; }
  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IExplorerCommand) *object = static_cast<IExplorerCommand*>(this);
    else { *object = nullptr; return E_NOINTERFACE; }
    AddRef(); return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  IFACEMETHODIMP_(ULONG) Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
  IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* title) override {
    return SHStrDupW(russian_ui() ? L"Открыть в Listopad++" : L"Open in Listopad++", title);
  }
  IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
    const std::wstring value = editor_path().wstring() + L",0";
    return SHStrDupW(value.c_str(), icon);
  }
  IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
    return SHStrDupW(russian_ui() ? L"Открыть выбранные файлы в Listopad++"
                                  : L"Open selected files in Listopad++", tip);
  }
  IFACEMETHODIMP GetCanonicalName(GUID* guid) override { if (!guid) return E_POINTER; *guid = kListopadCommand; return S_OK; }
  IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
    if (!state) return E_POINTER; DWORD count = 0;
    *state = items && SUCCEEDED(items->GetCount(&count)) && count ? ECS_ENABLED : ECS_DISABLED; return S_OK;
  }
  IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
    if (!items) return E_INVALIDARG;
    std::wstring command = listopad::quote_command_line_argument(editor_path().wstring());
    DWORD count = 0; items->GetCount(&count);
    for (DWORD index = 0; index < count; ++index) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(items->GetItemAt(index, &item))) {
        wchar_t* path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          command += L" " + listopad::quote_command_line_argument(path); CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    std::wstring mutable_command = command;
    const auto directory = editor_directory();
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &startup, &process)) return HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(process.hThread); CloseHandle(process.hProcess); return S_OK;
  }
  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override { if (!flags) return E_POINTER; *flags = ECF_DEFAULT; return S_OK; }
  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override { if (commands) *commands = nullptr; return E_NOTIMPL; }
 private:
  std::atomic_ulong references_{1};
};

class Factory final : public IClassFactory {
 public:
  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == IID_IClassFactory) *object = static_cast<IClassFactory*>(this);
    else { *object = nullptr; return E_NOINTERFACE; }
    AddRef(); return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }
  IFACEMETHODIMP_(ULONG) Release() override { const ULONG value = --references_; if (!value) delete this; return value; }
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto* command = new (std::nothrow) ExplorerCommand;
    if (!command) return E_OUTOFMEMORY;
    const HRESULT result = command->QueryInterface(iid, object); command->Release(); return result;
  }
  IFACEMETHODIMP LockServer(BOOL lock) override { lock ? ++g_objects : --g_objects; return S_OK; }
 private:
  std::atomic_ulong references_{1};
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) { g_instance = instance; DisableThreadLibraryCalls(instance); }
  return TRUE;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
  if (!IsEqualCLSID(clsid, kListopadCommand)) return CLASS_E_CLASSNOTAVAILABLE;
  auto* factory = new (std::nothrow) Factory;
  if (!factory) return E_OUTOFMEMORY;
  const HRESULT result = factory->QueryInterface(iid, object); factory->Release(); return result;
}
