#include "editor_window.h"
#include "single_instance.h"
#include "resource.h"

#include "listopad/command_line.h"
#include "listopad/ipc_protocol.h"
#include "listopad/settings.h"
#include "listopad/shell_registration.h"

#include <windows.h>

#include <iterator>
#include <utility>

namespace {
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
      {FVIRTKEY | FCONTROL, 'F', IDM_SEARCH_FIND}, {FVIRTKEY | FCONTROL, 'H', IDM_SEARCH_REPLACE},
      {FVIRTKEY, VK_F3, IDM_SEARCH_NEXT}, {FVIRTKEY | FALT | FSHIFT, 'F', IDM_TOOLS_FORMAT}};
  return CreateAcceleratorTableW(const_cast<ACCEL*>(entries), static_cast<int>(std::size(entries)));
}
}  // namespace

int WINAPI wWinMain(HINSTANCE module, HINSTANCE, wchar_t*, int show_command) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  listopad::record_executable_location();
  const listopad::CommandLine command = listopad::current_command_line();
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

  listopad::app::EditorWindow editor(module, std::move(settings));
  if (!editor.create(show_command)) return static_cast<int>(GetLastError());
  instance.start([window = editor.handle()](listopad::ipc::OpenFilesRequest incoming) {
    auto* copy = new listopad::ipc::OpenFilesRequest(std::move(incoming));
    if (!PostMessageW(window, listopad::app::EditorWindow::kOpenRequestMessage, 0,
                      reinterpret_cast<LPARAM>(copy))) delete copy;
  });
  editor.open_request(initial_request);

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
