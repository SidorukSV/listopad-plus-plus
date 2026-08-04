#include "recovery_dialog.h"

#include "resource.h"
#include "listopad/strings.h"

#include <commctrl.h>

#include <string>
#include <utility>

namespace listopad::app {
namespace {

struct DialogState {
  const std::vector<RecoverySnapshot>* snapshots{};
  bool russian{true};
  RecoveryChoice choice;
};

std::wstring item_label(const RecoverySnapshot& snapshot) {
  std::wstring label = snapshot.title;
  if (!snapshot.path.empty()) {
    if (!label.empty()) label += L" — ";
    label += snapshot.path.wstring();
  }
  return label.empty() ? L"Recovered document" : label;
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  auto* state = reinterpret_cast<DialogState*>(
      GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<DialogState*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER,
                      reinterpret_cast<LONG_PTR>(state));
    if (!state->russian) {
      SetWindowTextW(dialog, L"Listopad++ recovery");
      SetDlgItemTextW(
          dialog, IDC_RECOVERY_DESCRIPTION,
          L"The previous session ended unexpectedly. Select documents "
          L"to restore:");
      SetDlgItemTextW(dialog, IDOK, L"Restore selected");
      SetDlgItemTextW(dialog, IDC_RECOVERY_DELETE_ALL, L"Delete all");
      SetDlgItemTextW(dialog, IDCANCEL, L"Exit");
    }
    const HWND list = GetDlgItem(dialog, IDC_RECOVERY_LIST);
    for (std::size_t index = 0; index < state->snapshots->size();
         ++index) {
      const std::wstring label =
          item_label((*state->snapshots)[index]);
      const LRESULT item = SendMessageW(
          list, LB_ADDSTRING, 0,
          reinterpret_cast<LPARAM>(label.c_str()));
      if (item != LB_ERR && item != LB_ERRSPACE) {
        SendMessageW(list, LB_SETITEMDATA,
                     static_cast<WPARAM>(item),
                     static_cast<LPARAM>(index));
      }
    }
    SendMessageW(list, LB_SETSEL, TRUE, -1);
    return TRUE;
  }
  if (!state) return FALSE;
  if (message == WM_COMMAND) {
    const int command = LOWORD(wparam);
    if (command == IDOK) {
      const HWND list = GetDlgItem(dialog, IDC_RECOVERY_LIST);
      const int count =
          static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
      for (int item = 0; item < count; ++item) {
        if (SendMessageW(list, LB_GETSEL, item, 0) <= 0) continue;
        const auto index = static_cast<std::size_t>(
            SendMessageW(list, LB_GETITEMDATA, item, 0));
        if (index < state->snapshots->size()) {
          state->choice.selected_ids.push_back(
              (*state->snapshots)[index].id);
        }
      }
      if (state->choice.selected_ids.empty()) {
        MessageBoxW(
            dialog,
            state->russian ? L"Выберите хотя бы один документ."
                           : L"Select at least one document.",
            L"Listopad++", MB_OK | MB_ICONINFORMATION);
        return TRUE;
      }
      state->choice.action = RecoveryAction::RestoreSelected;
      EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (command == IDC_RECOVERY_DELETE_ALL) {
      const int answer = MessageBoxW(
          dialog,
          state->russian ? L"Удалить все аварийные копии?"
                         : L"Delete all recovery snapshots?",
          L"Listopad++", MB_YESNO | MB_ICONWARNING);
      if (answer == IDYES) {
        state->choice.action = RecoveryAction::DeleteAll;
        EndDialog(dialog, IDC_RECOVERY_DELETE_ALL);
      }
      return TRUE;
    }
    if (command == IDCANCEL) {
      state->choice.action = RecoveryAction::ExitPreserve;
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  return FALSE;
}

}  // namespace

RecoveryChoice RecoveryDialog::show(
    HWND owner, HINSTANCE instance,
    const std::vector<RecoverySnapshot>& snapshots,
    const bool russian) {
  DialogState state{&snapshots, russian, {}};
  DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_RECOVERY), owner,
                  dialog_proc, reinterpret_cast<LPARAM>(&state));
  return std::move(state.choice);
}

}  // namespace listopad::app
