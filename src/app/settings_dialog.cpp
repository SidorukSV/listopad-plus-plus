#include "settings_dialog.h"

#include "resource.h"
#include "listopad/strings.h"
#include "listopad/version.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace listopad::app {
namespace {

struct DialogState {
  Settings working;
  Settings applied;
  SettingsActions actions;
  SettingsDialog::ApplyCallback apply;
};

bool russian(const DialogState& state) {
  return state.working.ui_language != "en";
}

const wchar_t* tr(const DialogState& state, const wchar_t* ru,
                  const wchar_t* en) {
  return russian(state) ? ru : en;
}

void add_combo(HWND dialog, const int id,
               const std::initializer_list<const wchar_t*> labels,
               const int selection) {
  const HWND combo = GetDlgItem(dialog, id);
  for (const wchar_t* label : labels) {
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(label));
  }
  SendMessageW(combo, CB_SETCURSEL, selection, 0);
}

void set_number(HWND dialog, const int id, const std::uint64_t value) {
  SetDlgItemInt(dialog, id, static_cast<UINT>(value), FALSE);
}

int language_index(const std::string& value) {
  return value == "en" ? 1 : 0;
}

int theme_index(const std::string& value) {
  if (value == "light") return 1;
  if (value == "dark") return 2;
  return 0;
}

int encoding_index(const std::string& value) {
  constexpr std::array names{"auto", "utf-8", "windows-1251",
                             "cp866", "windows-1252"};
  const auto found = std::find(names.begin(), names.end(), value);
  return found == names.end()
             ? 0
             : static_cast<int>(std::distance(names.begin(), found));
}

void initialize(HWND dialog, DialogState& state) {
  add_combo(dialog, IDC_SETTINGS_LANGUAGE, {L"Русский", L"English"},
            language_index(state.working.ui_language));
  add_combo(dialog, IDC_SETTINGS_THEME,
            {L"Системная / System", L"Светлая / Light",
             L"Тёмная / Dark"},
            theme_index(state.working.theme));
  add_combo(dialog, IDC_SETTINGS_FALLBACK_ENCODING,
            {L"Авто / Auto", L"UTF-8", L"Windows-1251",
             L"DOS CP866", L"Windows-1252"},
            encoding_index(state.working.fallback_encoding));
  SetDlgItemTextW(dialog, IDC_SETTINGS_FONT_FACE,
                  utf8_to_wide(state.working.font_face).c_str());
  set_number(dialog, IDC_SETTINGS_FONT_SIZE, state.working.font_size);
  set_number(dialog, IDC_SETTINGS_INDENT_SIZE, state.working.indent_size);
  set_number(dialog, IDC_SETTINGS_LARGE_FILE_MB,
             state.working.large_file_threshold >> 20);
  set_number(dialog, IDC_SETTINGS_RECOVERY_MAX_MB,
             state.working.recovery_max_bytes >> 20);
  set_number(dialog, IDC_SETTINGS_RETENTION_DAYS,
             state.working.recovery_retention_days);
  CheckDlgButton(dialog, IDC_SETTINGS_INDENT_TABS,
                 state.working.indent_with_tabs ? BST_CHECKED
                                                : BST_UNCHECKED);
  CheckDlgButton(dialog, IDC_SETTINGS_DOCUMENT_MAP,
                 state.working.show_document_map ? BST_CHECKED
                                                 : BST_UNCHECKED);
  CheckDlgButton(dialog, IDC_SETTINGS_RECOVERY_ENABLED,
                 state.working.recovery_enabled ? BST_CHECKED
                                                : BST_UNCHECKED);
  CheckDlgButton(dialog, IDC_SETTINGS_RESTORE_SESSION,
                 state.working.restore_session ? BST_CHECKED
                                               : BST_UNCHECKED);
}

bool read_number(HWND dialog, const int id, const UINT minimum,
                 const UINT maximum, int& target) {
  BOOL ok = FALSE;
  const UINT value = GetDlgItemInt(dialog, id, &ok, FALSE);
  if (!ok || value < minimum || value > maximum) return false;
  target = static_cast<int>(value);
  return true;
}

bool read_settings(HWND dialog, DialogState& state) {
  Settings next = state.working;
  const auto combo = [dialog](const int id) {
    return static_cast<int>(
        SendDlgItemMessageW(dialog, id, CB_GETCURSEL, 0, 0));
  };
  next.ui_language =
      combo(IDC_SETTINGS_LANGUAGE) == 1 ? "en" : "ru";
  constexpr std::array themes{"system", "light", "dark"};
  const int theme = std::clamp(combo(IDC_SETTINGS_THEME), 0, 2);
  next.theme = themes[static_cast<std::size_t>(theme)];
  constexpr std::array encodings{
      "auto", "utf-8", "windows-1251", "cp866", "windows-1252"};
  const int encoding =
      std::clamp(combo(IDC_SETTINGS_FALLBACK_ENCODING), 0, 4);
  next.fallback_encoding =
      encodings[static_cast<std::size_t>(encoding)];

  std::wstring font(256, L'\0');
  const int font_length = GetDlgItemTextW(
      dialog, IDC_SETTINGS_FONT_FACE, font.data(),
      static_cast<int>(font.size()));
  font.resize(static_cast<std::size_t>((std::max)(font_length, 0)));
  if (font.empty()) {
    MessageBoxW(dialog,
                tr(state, L"Укажите имя шрифта.",
                   L"Enter a font family."),
                LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONWARNING);
    return false;
  }
  next.font_face = wide_to_utf8(font);

  int font_size = 0;
  int indent_size = 0;
  int large_mb = 0;
  int recovery_mb = 0;
  int retention = 0;
  if (!read_number(dialog, IDC_SETTINGS_FONT_SIZE, 7, 40,
                   font_size) ||
      !read_number(dialog, IDC_SETTINGS_INDENT_SIZE, 1, 8,
                   indent_size) ||
      !read_number(dialog, IDC_SETTINGS_LARGE_FILE_MB, 16, 4096,
                   large_mb) ||
      !read_number(dialog, IDC_SETTINGS_RECOVERY_MAX_MB, 1, 128,
                   recovery_mb) ||
      !read_number(dialog, IDC_SETTINGS_RETENTION_DAYS, 1, 30,
                   retention)) {
    MessageBoxW(
        dialog,
        tr(state,
           L"Проверьте числовые значения и допустимые диапазоны.",
           L"Check the numeric values and their allowed ranges."),
        LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONWARNING);
    return false;
  }
  next.font_size = font_size;
  next.indent_size = indent_size;
  next.large_file_threshold =
      static_cast<std::uint64_t>(large_mb) << 20;
  next.recovery_max_bytes =
      static_cast<std::uint64_t>(recovery_mb) << 20;
  next.recovery_retention_days = retention;
  next.indent_with_tabs =
      IsDlgButtonChecked(dialog, IDC_SETTINGS_INDENT_TABS) ==
      BST_CHECKED;
  next.show_document_map =
      IsDlgButtonChecked(dialog, IDC_SETTINGS_DOCUMENT_MAP) ==
      BST_CHECKED;
  next.recovery_enabled =
      IsDlgButtonChecked(dialog, IDC_SETTINGS_RECOVERY_ENABLED) ==
      BST_CHECKED;
  next.restore_session =
      IsDlgButtonChecked(dialog, IDC_SETTINGS_RESTORE_SESSION) ==
      BST_CHECKED;

  if (state.applied.recovery_enabled && !next.recovery_enabled &&
      !state.actions.clear_recovery) {
    if (MessageBoxW(
            dialog,
            tr(state,
               L"Отключение recovery удалит сохранённые аварийные "
               L"копии. Продолжить?",
               L"Disabling recovery deletes saved recovery snapshots. "
               L"Continue?"),
            LISTOPAD_PRODUCT_NAME,
            MB_YESNO | MB_ICONWARNING) != IDYES) {
      return false;
    }
    state.actions.clear_recovery = true;
  }
  state.working = std::move(next);
  return true;
}

bool apply(HWND dialog, DialogState& state) {
  if (!read_settings(dialog, state)) return false;
  if (!state.apply(state.working, state.actions)) {
    MessageBoxW(dialog,
                tr(state, L"Не удалось сохранить настройки.",
                   L"Unable to save settings."),
                LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONERROR);
    return false;
  }
  state.applied = state.working;
  state.actions = {};
  return true;
}

void request_clear(HWND dialog, DialogState& state, const int command) {
  const wchar_t* prompt = L"";
  if (command == IDC_SETTINGS_CLEAR_RECOVERY) {
    prompt = tr(state, L"Удалить все аварийные копии?",
                L"Delete all recovery snapshots?");
  } else if (command == IDC_SETTINGS_CLEAR_SESSION) {
    prompt = tr(state, L"Очистить сохранённое состояние сеанса?",
                L"Clear the saved session state?");
  } else {
    prompt = tr(state, L"Очистить недавние и закрытые файлы?",
                L"Clear recent and closed file history?");
  }
  if (MessageBoxW(dialog, prompt, LISTOPAD_PRODUCT_NAME,
                  MB_YESNO | MB_ICONWARNING) != IDYES) {
    return;
  }
  if (command == IDC_SETTINGS_CLEAR_RECOVERY)
    state.actions.clear_recovery = true;
  else if (command == IDC_SETTINGS_CLEAR_SESSION)
    state.actions.clear_session = true;
  else
    state.actions.clear_history = true;
}

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  auto* state = reinterpret_cast<DialogState*>(
      GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) {
    state = reinterpret_cast<DialogState*>(lparam);
    SetWindowLongPtrW(dialog, DWLP_USER,
                      reinterpret_cast<LONG_PTR>(state));
    initialize(dialog, *state);
    return TRUE;
  }
  if (!state) return FALSE;
  if (message == WM_COMMAND) {
    const int command = LOWORD(wparam);
    if (command == IDC_SETTINGS_CLEAR_RECOVERY ||
        command == IDC_SETTINGS_CLEAR_SESSION ||
        command == IDC_SETTINGS_CLEAR_HISTORY) {
      request_clear(dialog, *state, command);
      return TRUE;
    }
    if (command == IDC_SETTINGS_APPLY) {
      (void)apply(dialog, *state);
      return TRUE;
    }
    if (command == IDOK) {
      if (apply(dialog, *state)) EndDialog(dialog, IDOK);
      return TRUE;
    }
    if (command == IDCANCEL) {
      EndDialog(dialog, IDCANCEL);
      return TRUE;
    }
  }
  return FALSE;
}

}  // namespace

void SettingsDialog::show(HWND owner, HINSTANCE instance,
                          const Settings& initial, ApplyCallback apply) {
  DialogState state{initial, initial, {}, std::move(apply)};
  DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                  dialog_proc, reinterpret_cast<LPARAM>(&state));
}

}  // namespace listopad::app
