#include "editor_window.h"
#include "single_instance.h"
#include "resource.h"

#include "listopad/command_line.h"
#include "listopad/ipc_protocol.h"
#include "listopad/settings.h"
#include "listopad/shell_registration.h"

#include <windows.h>
#include <objbase.h>

#include <iterator>
#include <utility>

namespace {
class ComApartment final {
 public:
  ComApartment()
      : initialized_(SUCCEEDED(CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {}
  ~ComApartment() {
    if (initialized_) CoUninitialize();
  }

 private:
  bool initialized_{false};
};

listopad::ipc::OpenFilesRequest make_request(const listopad::CommandLine& command) {
  listopad::ipc::OpenFilesRequest request;
  request.files = command.files;
  request.line = command.line.value_or(0);
  request.column = command.column.value_or(0);
  request.encoding = command.encoding.value_or("");
  return request;
}

HACCEL create_accelerators() {
  const ACCEL entries[]{
      {FVIRTKEY | FCONTROL, 'N', IDM_FILE_NEW}, {FVIRTKEY | FCONTROL, 'O', IDM_FILE_OPEN},
      {FVIRTKEY | FCONTROL, 'S', IDM_FILE_SAVE}, {FVIRTKEY | FCONTROL, 'W', IDM_FILE_CLOSE},
      {FVIRTKEY | FCONTROL | FSHIFT, 'T', IDM_FILE_REOPEN_CLOSED},
      {FVIRTKEY | FCONTROL, 'F', IDM_SEARCH_FIND}, {FVIRTKEY | FCONTROL, 'H', IDM_SEARCH_REPLACE},
      {FVIRTKEY, VK_F3, IDM_SEARCH_NEXT}, {FVIRTKEY | FALT | FSHIFT, 'F', IDM_TOOLS_FORMAT}};
  return CreateAcceleratorTableW(const_cast<ACCEL*>(entries), static_cast<int>(std::size(entries)));
}

bool wait_for_previous_instance(
    const listopad::ElevatedRestartRequest& request) {
  const HANDLE process =
      OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                  request.parent_process_id);
  if (!process) return false;

  std::wstring parent_path(32768, L'\0');
  DWORD parent_length = static_cast<DWORD>(parent_path.size());
  std::wstring current_path(32768, L'\0');
  const DWORD current_length =
      GetModuleFileNameW(nullptr, current_path.data(),
                         static_cast<DWORD>(current_path.size()));
  if (!QueryFullProcessImageNameW(process, 0, parent_path.data(),
                                  &parent_length) ||
      current_length == 0 || current_length >= current_path.size()) {
    CloseHandle(process);
    return false;
  }
  parent_path.resize(parent_length);
  current_path.resize(current_length);
  if (CompareStringOrdinal(parent_path.c_str(), -1, current_path.c_str(), -1,
                           TRUE) != CSTR_EQUAL) {
    CloseHandle(process);
    SetLastError(ERROR_INVALID_DATA);
    return false;
  }

  const HANDLE ready =
      OpenEventW(EVENT_MODIFY_STATE, FALSE, request.ready_event.c_str());
  if (!ready || !SetEvent(ready)) {
    if (ready) CloseHandle(ready);
    CloseHandle(process);
    return false;
  }
  CloseHandle(ready);

  const DWORD wait = WaitForSingleObject(process, 30'000);
  CloseHandle(process);
  if (wait == WAIT_OBJECT_0) return true;
  SetLastError(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_GEN_FAILURE);
  return false;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, wchar_t*, int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const ComApartment com_apartment;
  const listopad::CommandLine command = listopad::current_command_line();
  if (command.elevated_restart &&
      !wait_for_previous_instance(*command.elevated_restart)) {
    MessageBoxW(
        nullptr,
        L"Listopad++ could not wait for the previous process to exit.",
        L"Listopad++", MB_OK | MB_ICONERROR);
    return static_cast<int>(GetLastError());
  }
  listopad::Settings settings = listopad::load_settings();
  if (command.shell_registration == listopad::ShellRegistration::Register)
    return listopad::register_classic_context_menu(settings.ui_language) ? 0 : 1;
  if (command.shell_registration == listopad::ShellRegistration::Unregister)
    return listopad::unregister_classic_context_menu() ? 0 : 1;

  listopad::app::SingleInstance instance;
  const auto initial_request = make_request(command);
  if (!instance.primary()) {
    if (instance.send(initial_request)) return 0;
    MessageBoxW(nullptr,
                L"Listopad++ is already running, but is not responding to file-open requests.",
                L"Listopad++", MB_OK | MB_ICONERROR);
    return ERROR_PIPE_NOT_CONNECTED;
  }

  listopad::app::EditorWindow editor(
      module, std::move(settings), command.elevated_restart.has_value());
  if (!editor.create(show_command)) return static_cast<int>(GetLastError());
  instance.start([window = editor.handle()](listopad::ipc::OpenFilesRequest incoming) {
    auto* copy = new listopad::ipc::OpenFilesRequest(std::move(incoming));
    if (!PostMessageW(window, listopad::app::EditorWindow::kOpenRequestMessage, 0,
                      reinterpret_cast<LPARAM>(copy))) delete copy;
  });
  editor.open_request(initial_request);
  if (command.elevated_restart) {
    editor.retry_elevated_save(*command.elevated_restart);
  }

  HACCEL accelerators = create_accelerators();
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!TranslateAcceleratorW(editor.handle(), accelerators, &message)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
  }
  DestroyAcceleratorTable(accelerators);
  return static_cast<int>(message.wParam);
}
