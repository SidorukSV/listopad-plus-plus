#include "pff_profile_view.h"

#include "pff_profile_controller.h"
#include "listopad/file_io.h"
#include "listopad/strings.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace listopad::app {
namespace {

constexpr wchar_t kClassName[] = L"ListopadPPPffProfile";
constexpr UINT kLoadComplete = WM_APP + 100;
constexpr UINT kSetDark = WM_APP + 101;
constexpr int kSummaryId = 1;
constexpr int kFilterId = 2;
constexpr int kRecordsId = 3;
constexpr int kColumnCount = 9;

struct CreateOptions {
  bool russian{true};
  PffProfileUiState ui;
};

struct State {
  PffProfileController controller;
  PffProfileDocument document;
  std::vector<std::size_t> visible_records;
  HWND summary{nullptr};
  HWND filter{nullptr};
  HWND records{nullptr};
  HFONT font{nullptr};
  HBRUSH background_brush{nullptr};
  bool russian{true};
  bool dark{false};
  bool loaded{false};
  std::wstring status;
  PffProfileUiState ui;

  ~State() {
    if (background_brush) DeleteObject(background_brush);
  }
};

State* state_for(const HWND window) noexcept {
  return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void copy_wide_text(const std::wstring_view text) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (memory) {
    void* data = GlobalLock(memory);
    if (data) {
      std::memcpy(data, text.data(), text.size() * sizeof(wchar_t));
      static_cast<wchar_t*>(data)[text.size()] = L'\0';
      GlobalUnlock(memory);
      if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}

const PffProfileRecord* selected_record(const State& state,
                                        int* visible_index = nullptr) {
  if (!state.loaded || state.visible_records.empty()) return nullptr;
  int selected = ListView_GetNextItem(state.records, -1, LVNI_SELECTED);
  if (selected < 0 || static_cast<std::size_t>(selected) >=
                          state.visible_records.size()) {
    const auto found = std::find(state.visible_records.begin(),
                                 state.visible_records.end(),
                                 state.ui.selected_record);
    if (found == state.visible_records.end()) return nullptr;
    selected = static_cast<int>(
        std::distance(state.visible_records.begin(), found));
  }
  if (visible_index) *visible_index = selected;
  const std::size_t record_index =
      state.visible_records[static_cast<std::size_t>(selected)];
  return record_index < state.document.records.size()
             ? &state.document.records[record_index]
             : nullptr;
}

int compare_records(const PffProfileRecord& left,
                    const PffProfileRecord& right, const int column) {
  const auto compare = [](const auto& a, const auto& b) {
    return a < b ? -1 : b < a ? 1 : 0;
  };
  switch (column) {
    case 0:
      return compare(left.module, right.module);
    case 1:
      return compare(left.line, right.line);
    case 2:
      return compare(left.code, right.code);
    case 3:
      return compare(left.calls, right.calls);
    case 4:
      return compare(left.inclusive_time, right.inclusive_time);
    case 5:
      return compare(left.self_time, right.self_time);
    case 6:
      return compare(left.inclusive_share, right.inclusive_share);
    case 7:
      return compare(left.self_share, right.self_share);
    case 8:
      return compare(std::array{left.client, left.server, left.server_call},
                     std::array{right.client, right.server,
                                right.server_call});
    default:
      return 0;
  }
}

void update_sort_arrow(const State& state) {
  HWND header = ListView_GetHeader(state.records);
  if (!header) return;
  const int count = Header_GetItemCount(header);
  for (int index = 0; index < count; ++index) {
    HDITEMW item{};
    item.mask = HDI_FORMAT;
    if (!Header_GetItem(header, index, &item)) continue;
    item.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
    if (index == state.ui.sort_column) {
      item.fmt |= state.ui.sort_descending ? HDF_SORTDOWN : HDF_SORTUP;
    }
    Header_SetItem(header, index, &item);
  }
}

void rebuild_visible(State& state) {
  const std::size_t keep = state.ui.selected_record;
  state.visible_records.clear();
  if (state.loaded) {
    for (std::size_t index = 0; index < state.document.records.size();
         ++index) {
      if (pff_profile_record_matches_filter(state.document.records[index],
                                            state.ui.filter)) {
        state.visible_records.push_back(index);
      }
    }
    const int direction = state.ui.sort_descending ? -1 : 1;
    std::stable_sort(
        state.visible_records.begin(), state.visible_records.end(),
        [&state, direction](const std::size_t left,
                            const std::size_t right) {
          const int order = compare_records(state.document.records[left],
                                            state.document.records[right],
                                            state.ui.sort_column);
          return order == 0 ? left < right : order * direction < 0;
        });
  }
  ListView_SetItemCountEx(
      state.records,
      static_cast<int>((std::min)(
          state.visible_records.size(),
          static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
      LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  if (!state.visible_records.empty()) {
    const auto found = std::find(state.visible_records.begin(),
                                 state.visible_records.end(), keep);
    const int row = found == state.visible_records.end()
                        ? 0
                        : static_cast<int>(std::distance(
                              state.visible_records.begin(), found));
    state.ui.selected_record =
        state.visible_records[static_cast<std::size_t>(row)];
    ListView_SetItemState(state.records, row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(state.records, row);
    ListView_EnsureVisible(state.records, row, FALSE);
  }
  update_sort_arrow(state);
  InvalidateRect(state.records, nullptr, TRUE);
}

void update_summary(State& state) {
  if (!state.loaded) {
    SetWindowTextW(state.summary, state.status.c_str());
    return;
  }
  std::wstring text = state.russian ? L"Замер производительности 1С"
                                    : L"1C performance profile";
  text += state.russian ? L" · строк: " : L" · rows: ";
  text += std::to_wstring(state.document.records.size());
  text += state.russian ? L" · модулей: " : L" · modules: ";
  text += std::to_wstring(state.document.module_count);
  text += state.russian ? L" · чистое время: " : L" · self time: ";
  text += utf8_to_wide(pff_profile_format_time(
      state.document.total_self_time, state.russian));
  if (state.document.skipped_records != 0) {
    text += state.russian ? L" · пропущено: " : L" · skipped: ";
    text += std::to_wstring(state.document.skipped_records);
  }
  SetWindowTextW(state.summary, text.c_str());
}

void apply_layout(const HWND window, State& state) {
  RECT client{};
  GetClientRect(window, &client);
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const PffProfileLayout layout = calculate_pff_profile_layout(
      client.right - client.left, client.bottom - client.top, dpi);
  const auto move = [](const HWND control, const LayoutRect& rect) {
    MoveWindow(control, rect.x, rect.y, rect.width, rect.height, TRUE);
  };
  move(state.summary, layout.summary);
  move(state.filter, layout.filter);
  move(state.records, layout.records);

  const std::array<int, kColumnCount> widths{
      MulDiv(255, dpi, 96), MulDiv(62, dpi, 96),
      MulDiv(360, dpi, 96), MulDiv(78, dpi, 96),
      MulDiv(105, dpi, 96), MulDiv(105, dpi, 96),
      MulDiv(92, dpi, 96),  MulDiv(92, dpi, 96),
      MulDiv(150, dpi, 96)};
  for (int index = 0; index < kColumnCount; ++index) {
    ListView_SetColumnWidth(state.records, index, widths[index]);
  }
}

void apply_theme(const HWND window, State& state) {
  if (state.background_brush) DeleteObject(state.background_brush);
  const COLORREF background =
      state.dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW);
  const COLORREF foreground =
      state.dark ? RGB(225, 225, 225) : GetSysColor(COLOR_WINDOWTEXT);
  state.background_brush = CreateSolidBrush(background);
  ListView_SetBkColor(state.records, background);
  ListView_SetTextBkColor(state.records, background);
  ListView_SetTextColor(state.records, foreground);
  for (const HWND control : {state.filter, state.records}) {
    SetWindowTheme(control, state.dark ? L"DarkMode_Explorer" : nullptr,
                   nullptr);
  }
  InvalidateRect(window, nullptr, TRUE);
}

std::wstring cell_text(const State& state, const PffProfileRecord& record,
                       const int column) {
  switch (column) {
    case 0:
      return utf8_to_wide(record.module);
    case 1:
      return std::to_wstring(record.line);
    case 2:
      return utf8_to_wide(record.code);
    case 3:
      return std::to_wstring(record.calls);
    case 4:
      return utf8_to_wide(
          pff_profile_format_time(record.inclusive_time, state.russian));
    case 5:
      return utf8_to_wide(
          pff_profile_format_time(record.self_time, state.russian));
    case 6:
      return utf8_to_wide(
          pff_profile_format_share(record.inclusive_share, state.russian));
    case 7:
      return utf8_to_wide(
          pff_profile_format_share(record.self_share, state.russian));
    case 8:
      return utf8_to_wide(pff_profile_context(record, state.russian));
    default:
      return {};
  }
}

void provide_text(State& state, NMLVDISPINFOW& display) {
  if ((display.item.mask & LVIF_TEXT) == 0 || display.item.iItem < 0 ||
      static_cast<std::size_t>(display.item.iItem) >=
          state.visible_records.size() ||
      !display.item.pszText || display.item.cchTextMax <= 0) {
    return;
  }
  const std::size_t index =
      state.visible_records[static_cast<std::size_t>(display.item.iItem)];
  if (index >= state.document.records.size()) return;
  const std::wstring text =
      cell_text(state, state.document.records[index], display.item.iSubItem);
  wcsncpy_s(display.item.pszText,
            static_cast<std::size_t>(display.item.cchTextMax), text.c_str(),
            _TRUNCATE);
}

std::wstring record_as_text(const State& state,
                            const PffProfileRecord& record) {
  std::wstring result = state.russian
                            ? L"Модуль\tСтрока\tКод\tВызовы\tВремя\tЧистое "
                              L"время\tДоля\tЧистая доля\tКонтекст\r\n"
                            : L"Module\tLine\tCode\tCalls\tTime\tSelf time\t"
                              L"Share\tSelf share\tContext\r\n";
  for (int column = 0; column < kColumnCount; ++column) {
    if (column != 0) result.push_back(L'\t');
    result += cell_text(state, record, column);
  }
  result += L"\r\n";
  return result;
}

}  // namespace

bool PffProfileView::register_class(const HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance;
  type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  type.hbrBackground = nullptr;
  return RegisterClassExW(&type) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND PffProfileView::create(const HWND parent, const int control_id,
                            const bool russian,
                            const PffProfileUiState initial_state) {
  CreateOptions options{.russian = russian, .ui = initial_state};
  return CreateWindowExW(
      0, kClassName, L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
      0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
      GetModuleHandleW(nullptr), &options);
}

bool PffProfileView::open(const HWND window,
                          const std::filesystem::path& path) {
  State* state = state_for(window);
  if (!state) return false;
  state->loaded = false;
  state->document = {};
  state->visible_records.clear();
  state->status = state->russian ? L"Замер 1С · чтение…"
                                 : L"1C profile · reading…";
  ListView_SetItemCount(state->records, 0);
  update_summary(*state);
  state->controller.start(window, kLoadComplete, path);
  return true;
}

bool PffProfileView::looks_like(const std::filesystem::path& path) {
  std::wstring extension = path.extension().wstring();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](const wchar_t value) {
                   return static_cast<wchar_t>(std::towlower(value));
                 });
  if (extension == L".pff") return true;
  MappedFile file;
  if (!file.open(path) || !file.data()) return false;
  const auto sample_size = static_cast<std::size_t>(
      (std::min)(file.size(), static_cast<std::uint64_t>(64 * 1024)));
  return looks_like_pff_profile(
      {reinterpret_cast<const char*>(file.data()), sample_size});
}

void PffProfileView::set_dark(const HWND window, const bool dark) {
  SendMessageW(window, kSetDark, dark ? TRUE : FALSE, 0);
}

bool PffProfileView::find_next(const HWND window,
                               const std::string& pattern,
                               const SearchOptions options,
                               const bool wrap) {
  State* state = state_for(window);
  if (!state || !state->loaded || pattern.empty() ||
      state->visible_records.empty()) {
    return false;
  }
  int selected = -1;
  (void)selected_record(*state, &selected);
  const int count = static_cast<int>(state->visible_records.size());
  const int first = selected >= 0 ? selected + 1 : 0;
  const int attempts = wrap ? count : count - first;
  for (int offset = 0; offset < attempts; ++offset) {
    const int row = (first + offset) % count;
    const PffProfileRecord& record = state->document.records[
        state->visible_records[static_cast<std::size_t>(row)]];
    const std::string subject = record.module + ' ' + record.code;
    const SearchOneResult found =
        search_next(subject, pattern, options, 0, false);
    if (!found.ok || !found.found) continue;
    ListView_SetItemState(state->records, row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state->records, row, FALSE);
    SetFocus(state->records);
    return true;
  }
  MessageBeep(MB_ICONINFORMATION);
  return false;
}

bool PffProfileView::copy(const HWND window) {
  State* state = state_for(window);
  if (!state) return false;
  const PffProfileRecord* record = selected_record(*state);
  if (!record) return false;
  copy_wide_text(record_as_text(*state, *record));
  return true;
}

bool PffProfileView::select_all(const HWND) {
  return false;
}

PffProfileUiState PffProfileView::ui_state(const HWND window) {
  const State* state = state_for(window);
  return state ? state->ui : PffProfileUiState{};
}

LRESULT CALLBACK PffProfileView::window_proc(const HWND window,
                                             const UINT message,
                                             const WPARAM wparam,
                                             const LPARAM lparam) {
  State* state = state_for(window);
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    const auto* options =
        static_cast<const CreateOptions*>(create->lpCreateParams);
    state = new State;
    if (options) {
      state->russian = options->russian;
      state->ui = options->ui;
    }
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
  } else if (message == WM_NCDESTROY) {
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wparam, lparam);
  } else if (message == WM_CREATE && state) {
    state->summary = CreateWindowExW(
        0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_CENTERIMAGE, 0, 0, 0,
        0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSummaryId)),
        nullptr, nullptr);
    state->filter = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterId)), nullptr,
        nullptr);
    state->records = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRecordsId)), nullptr,
        nullptr);
    ListView_SetExtendedListViewStyle(
        state->records, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                            LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);

    const std::array<const wchar_t*, 5> filters =
        state->russian
            ? std::array<const wchar_t*, 5>{
                  L"Все строки", L"Горячие (чистая доля ≥ 1 %)",
                  L"Клиент", L"Сервер", L"Вызовы сервера"}
            : std::array<const wchar_t*, 5>{
                  L"All rows", L"Hot (self share ≥ 1%)", L"Client",
                  L"Server", L"Server calls"};
    for (const wchar_t* filter : filters) {
      SendMessageW(state->filter, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(filter));
    }
    SendMessageW(state->filter, CB_SETCURSEL,
                 static_cast<WPARAM>(state->ui.filter), 0);

    const std::array<const wchar_t*, kColumnCount> columns =
        state->russian
            ? std::array<const wchar_t*, kColumnCount>{
                  L"Модуль", L"Строка", L"Код", L"Вызовы", L"Время",
                  L"Чистое время", L"Доля", L"Чистая доля", L"Контекст"}
            : std::array<const wchar_t*, kColumnCount>{
                  L"Module", L"Line", L"Code", L"Calls", L"Time",
                  L"Self time", L"Share", L"Self share", L"Context"};
    for (int index = 0; index < kColumnCount; ++index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
      column.pszText = const_cast<wchar_t*>(columns[index]);
      column.iSubItem = index;
      column.cx = 100;
      ListView_InsertColumn(state->records, index, &column);
    }
    apply_theme(window, *state);
    apply_layout(window, *state);
    update_sort_arrow(*state);
    return 0;
  } else if (message == WM_SIZE && state) {
    apply_layout(window, *state);
    return 0;
  } else if (message == WM_SETFONT && state) {
    state->font = reinterpret_cast<HFONT>(wparam);
    for (const HWND control : {state->summary, state->filter,
                               state->records}) {
      SendMessageW(control, WM_SETFONT, wparam, TRUE);
    }
    return 0;
  } else if (message == WM_SETFOCUS && state) {
    SetFocus(state->records);
    return 0;
  } else if (message == kSetDark && state) {
    state->dark = wparam != FALSE;
    apply_theme(window, *state);
    return 0;
  } else if (message == kLoadComplete && state) {
    std::unique_ptr<PffProfileLoadResult> completed(
        reinterpret_cast<PffProfileLoadResult*>(lparam));
    if (!completed || !state->controller.accepts(*completed)) return 0;
    if (completed->error != 0) {
      state->status =
          (state->russian ? L"Не удалось прочитать замер: "
                          : L"Unable to read the profile: ") +
          win32_error_message(completed->error);
      update_summary(*state);
      return 0;
    }
    state->document = std::move(completed->document);
    state->loaded = true;
    update_summary(*state);
    rebuild_visible(*state);
    return 0;
  } else if (message == WM_COMMAND && state) {
    if (LOWORD(wparam) == kFilterId && HIWORD(wparam) == CBN_SELCHANGE) {
      const LRESULT selected =
          SendMessageW(state->filter, CB_GETCURSEL, 0, 0);
      state->ui.filter =
          selected >= 0 && selected <=
                               static_cast<LRESULT>(PffProfileFilter::ServerCalls)
              ? static_cast<PffProfileFilter>(selected)
              : PffProfileFilter::All;
      rebuild_visible(*state);
      return 0;
    }
  } else if (message == WM_NOTIFY && state) {
    const auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header && header->hwndFrom == state->records) {
      if (header->code == LVN_GETDISPINFOW) {
        provide_text(*state, *reinterpret_cast<NMLVDISPINFOW*>(lparam));
        return 0;
      }
      if (header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((change->uChanged & LVIF_STATE) != 0 &&
            (change->uNewState & LVIS_SELECTED) != 0 &&
            change->iItem >= 0 &&
            static_cast<std::size_t>(change->iItem) <
                state->visible_records.size()) {
          state->ui.selected_record = state->visible_records[
              static_cast<std::size_t>(change->iItem)];
        }
        return 0;
      }
      if (header->code == LVN_COLUMNCLICK) {
        const auto* click = reinterpret_cast<NMLISTVIEW*>(lparam);
        if (click->iSubItem == state->ui.sort_column) {
          state->ui.sort_descending = !state->ui.sort_descending;
        } else {
          state->ui.sort_column = click->iSubItem;
          state->ui.sort_descending = click->iSubItem >= 3;
        }
        rebuild_visible(*state);
        return 0;
      }
    }
  } else if (message == WM_CTLCOLORSTATIC && state) {
    const HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, state->dark ? RGB(225, 225, 225)
                                : GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(dc, state->dark ? RGB(30, 30, 30)
                              : GetSysColor(COLOR_WINDOW));
    return reinterpret_cast<LRESULT>(state->background_brush);
  } else if (message == WM_ERASEBKGND && state) {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(reinterpret_cast<HDC>(wparam), &client,
             state->background_brush ? state->background_brush
                                     : GetSysColorBrush(COLOR_WINDOW));
    return 1;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace listopad::app
