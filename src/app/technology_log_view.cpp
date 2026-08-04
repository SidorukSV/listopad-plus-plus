#include "technology_log_view.h"

#include "technology_log_controller.h"
#include "listopad/strings.h"
#include "listopad/technology_log.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace listopad::app {
namespace {

constexpr wchar_t kClassName[] = L"ListopadPPTechnologyLog";
constexpr UINT kLoadComplete = WM_APP + 80;
constexpr UINT kSetDark = WM_APP + 81;
constexpr int kSummaryId = 1;
constexpr int kFilterId = 2;
constexpr int kEventsId = 3;
constexpr int kRawToggleId = 4;
constexpr int kDetailsId = 5;
constexpr int kRawId = 6;

struct CreateOptions {
  bool russian{true};
  TechnologyLogUiState ui;
};

struct State {
  TechnologyLogController controller;
  std::unique_ptr<TechnologyLogLoadResult> loaded;
  std::vector<std::size_t> visible_events;
  HWND summary{nullptr};
  HWND filter{nullptr};
  HWND events{nullptr};
  HWND raw_toggle{nullptr};
  HWND details{nullptr};
  HWND raw{nullptr};
  HFONT font{nullptr};
  HBRUSH background_brush{nullptr};
  std::filesystem::path path;
  bool russian{true};
  bool dark{false};
  TechnologyLogUiState ui;

  ~State() {
    if (background_brush) DeleteObject(background_brush);
  }
};

State* state_for(const HWND window) noexcept {
  return reinterpret_cast<State*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
}

std::string_view source_for(const State& state) noexcept {
  if (!state.loaded || !state.loaded->file.data() ||
      state.loaded->file.size() >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::size_t>::max)())) {
    return {};
  }
  return {
      reinterpret_cast<const char*>(state.loaded->file.data()),
      static_cast<std::size_t>(state.loaded->file.size()),
  };
}

bool ascii_iequal(const std::string_view left,
                  const std::string_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto lower = [](const unsigned char value) {
      return value >= 'A' && value <= 'Z'
                 ? static_cast<unsigned char>(value - 'A' + 'a')
                 : value;
    };
    if (lower(static_cast<unsigned char>(left[index])) !=
        lower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

std::wstring display_text(const std::string_view source) {
  std::wstring wide = utf8_to_wide(source);
  if (wide.empty() && !source.empty()) {
    const int size = MultiByteToWideChar(
        CP_ACP, 0, source.data(), static_cast<int>((std::min)(
            source.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
        nullptr, 0);
    if (size > 0) {
      wide.resize(static_cast<std::size_t>(size));
      MultiByteToWideChar(
          CP_ACP, 0, source.data(), static_cast<int>((std::min)(
              source.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
          wide.data(), size);
    }
  }
  std::wstring windows;
  windows.reserve(wide.size() + wide.size() / 16);
  for (std::size_t index = 0; index < wide.size(); ++index) {
    if (wide[index] == L'\n' &&
        (index == 0 || wide[index - 1] != L'\r')) {
      windows.push_back(L'\r');
    }
    windows.push_back(wide[index]);
  }
  return windows;
}

void set_control_text(const HWND control, const std::string_view text) {
  const std::wstring wide = display_text(text);
  SetWindowTextW(control, wide.c_str());
}

bool matches_filter(const State& state, const TechnologyLogEvent& event) {
  const std::string_view source = source_for(state);
  switch (state.ui.filter) {
    case TechnologyLogFilter::All:
      return true;
    case TechnologyLogFilter::Warnings: {
      const TechnologyLogInterpretation interpretation =
          interpret_technology_log_event(source, event, state.russian);
      return interpretation.kind !=
             TechnologyLogInterpretationKind::Information;
    }
    case TechnologyLogFilter::Slow:
      return event.duration_us >= 1'000'000;
    case TechnologyLogFilter::Http:
      return ascii_iequal(event.type, "VRSREQUEST") ||
             ascii_iequal(event.type, "VRSRESPONSE");
    case TechnologyLogFilter::Cache:
      return ascii_iequal(event.type, "VRSCACHE");
  }
  return true;
}

const TechnologyLogEvent* selected_event(const State& state,
                                         int* visible_index = nullptr) {
  if (!state.loaded || !state.events) return nullptr;
  int selected =
      ListView_GetNextItem(state.events, -1, LVNI_SELECTED);
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >= state.visible_events.size()) {
    const auto persisted = std::find(
        state.visible_events.begin(), state.visible_events.end(),
        state.ui.selected_event);
    if (persisted == state.visible_events.end()) return nullptr;
    selected = static_cast<int>(
        std::distance(state.visible_events.begin(), persisted));
  }
  if (visible_index) *visible_index = selected;
  const std::size_t index =
      state.visible_events[static_cast<std::size_t>(selected)];
  if (index >= state.loaded->document.events.size()) return nullptr;
  return &state.loaded->document.events[index];
}

void update_details(State& state) {
  int visible_index = -1;
  const TechnologyLogEvent* event =
      selected_event(state, &visible_index);
  if (!event) {
    SetWindowTextW(
        state.details,
        state.russian ? L"Выберите событие."
                      : L"Select an event.");
    SetWindowTextW(state.raw, L"");
    return;
  }
  if (visible_index >= 0) {
    state.ui.selected_event =
        state.visible_events[static_cast<std::size_t>(visible_index)];
  }

  const std::string_view source = source_for(state);
  const TechnologyLogInterpretation interpretation =
      interpret_technology_log_event(source, *event, state.russian);
  std::string details = interpretation.summary;
  details += "\r\n\r\n";
  details += state.russian ? "Время: " : "Time: ";
  details += technology_log_format_timestamp(state.loaded->document, *event);
  details += "\r\n";
  details += state.russian ? "Длительность: " : "Duration: ";
  details += technology_log_format_duration(event->duration_us, state.russian);
  details += "\r\n";
  details += state.russian ? "Событие: " : "Event: ";
  details += event->type;
  details += "\r\n";
  details += state.russian ? "Вложенность: " : "Nesting: ";
  details += std::to_string(event->nesting_level);
  if (event->incomplete) {
    details += state.russian
                   ? "\r\n\r\nВнимание: запись оборвана или содержит "
                     "незакрытое значение."
                   : "\r\n\r\nWarning: the record is incomplete or contains "
                     "an unterminated value.";
  }
  details += state.russian ? "\r\n\r\nПоля\r\n"
                           : "\r\n\r\nFields\r\n";
  for (const TechnologyLogField& field : event->fields) {
    const std::string_view name =
        technology_log_field_name(source, field);
    details += name;
    details += " = ";
    details += technology_log_field_value(source, *event, name);
    details += "\r\n";
  }
  set_control_text(state.details, details);
  set_control_text(
      state.raw, technology_log_raw_event(source, *event));
}

void update_summary(State& state) {
  if (!state.loaded) return;
  const auto& events = state.loaded->document.events;
  std::size_t warnings = 0;
  std::size_t slow = 0;
  const std::string_view source = source_for(state);
  for (const TechnologyLogEvent& event : events) {
    if (interpret_technology_log_event(source, event, state.russian).kind !=
        TechnologyLogInterpretationKind::Information) {
      ++warnings;
    }
    if (event.duration_us >= 1'000'000) ++slow;
  }
  std::wstring summary = L"ТЖ · ";
  if (!state.russian) summary = L"Technology log · ";
  summary += std::to_wstring(events.size());
  summary += state.russian ? L" событий" : L" events";
  summary += state.russian ? L" · предупреждений: " : L" · warnings: ";
  summary += std::to_wstring(warnings);
  summary += L" · >1 ";
  summary += state.russian ? L"с: " : L"s: ";
  summary += std::to_wstring(slow);
  if (!events.empty()) {
    summary += L" · ";
    const std::string first = technology_log_format_timestamp(
        state.loaded->document, events.front());
    std::string last = technology_log_format_timestamp(
        state.loaded->document, events.back());
    summary += utf8_to_wide(first);
    summary += L" — ";
    if (state.loaded->document.file_time.valid && last.size() > 11) {
      last.erase(0, 11);
    }
    summary += utf8_to_wide(last);
  }
  SetWindowTextW(state.summary, summary.c_str());
}

void rebuild_visible(State& state) {
  state.visible_events.clear();
  if (state.loaded) {
    const auto& events = state.loaded->document.events;
    state.visible_events.reserve(events.size());
    for (std::size_t index = 0; index < events.size(); ++index) {
      if (matches_filter(state, events[index])) {
        state.visible_events.push_back(index);
      }
    }
  }
  ListView_SetItemCountEx(
      state.events, static_cast<int>((std::min)(
                        state.visible_events.size(),
                        static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
      LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  if (!state.visible_events.empty()) {
    const auto selected = std::find(
        state.visible_events.begin(), state.visible_events.end(),
        state.ui.selected_event);
    const int selected_row =
        selected == state.visible_events.end()
            ? 0
            : static_cast<int>(
                  std::distance(state.visible_events.begin(), selected));
    ListView_SetItemState(
        state.events, selected_row, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(state.events, selected_row);
    ListView_EnsureVisible(state.events, selected_row, FALSE);
  }
  update_details(state);
}

void apply_layout(const HWND window, State& state) {
  RECT client{};
  GetClientRect(window, &client);
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const TechnologyLogLayout layout = calculate_technology_log_layout(
      client.right - client.left, client.bottom - client.top, dpi,
      state.ui.show_raw);
  const auto move = [](const HWND control, const LayoutRect& rect) {
    MoveWindow(control, rect.x, rect.y, rect.width, rect.height, TRUE);
  };
  move(state.summary, layout.summary);
  move(state.filter, layout.filter);
  move(state.events, layout.events);
  move(state.raw_toggle, layout.raw_toggle);
  move(state.details, layout.details);
  move(state.raw, layout.raw);
  ShowWindow(state.raw, state.ui.show_raw ? SW_SHOW : SW_HIDE);

  RECT table{};
  GetClientRect(state.events, &table);
  const int time_width = MulDiv(190, dpi, 96);
  const int duration_width = MulDiv(92, dpi, 96);
  const int event_width = MulDiv(110, dpi, 96);
  ListView_SetColumnWidth(state.events, 0, time_width);
  ListView_SetColumnWidth(state.events, 1, duration_width);
  ListView_SetColumnWidth(state.events, 2, event_width);
  const int available_width =
      static_cast<int>(table.right) - time_width - duration_width -
      event_width - GetSystemMetrics(SM_CXVSCROLL) - 6;
  const int interpretation_width =
      (std::max)(MulDiv(240, dpi, 96), available_width);
  ListView_SetColumnWidth(state.events, 3, interpretation_width);
}

void apply_theme(const HWND window, State& state) {
  if (state.background_brush) {
    DeleteObject(state.background_brush);
    state.background_brush = nullptr;
  }
  const COLORREF background =
      state.dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW);
  const COLORREF foreground =
      state.dark ? RGB(225, 225, 225) : GetSysColor(COLOR_WINDOWTEXT);
  state.background_brush = CreateSolidBrush(background);
  ListView_SetBkColor(state.events, background);
  ListView_SetTextBkColor(state.events, background);
  ListView_SetTextColor(state.events, foreground);
  for (const HWND control :
       {state.filter, state.events, state.raw_toggle, state.details,
        state.raw}) {
    SetWindowTheme(control, state.dark ? L"DarkMode_Explorer" : nullptr,
                   nullptr);
  }
  InvalidateRect(window, nullptr, TRUE);
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
      if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
      }
    } else {
      GlobalFree(memory);
    }
  }
  CloseClipboard();
}

void provide_list_text(State& state, NMLVDISPINFOW& display) {
  if ((display.item.mask & LVIF_TEXT) == 0 ||
      display.item.iItem < 0 ||
      static_cast<std::size_t>(display.item.iItem) >=
          state.visible_events.size() ||
      !state.loaded || !display.item.pszText ||
      display.item.cchTextMax <= 0) {
    return;
  }
  const std::size_t index =
      state.visible_events[static_cast<std::size_t>(display.item.iItem)];
  if (index >= state.loaded->document.events.size()) return;
  const TechnologyLogEvent& event =
      state.loaded->document.events[index];
  const std::string_view source = source_for(state);
  std::wstring text;
  switch (display.item.iSubItem) {
    case 0:
      {
        std::string timestamp = technology_log_format_timestamp(
            state.loaded->document, event);
        if (state.loaded->document.file_time.valid &&
            timestamp.size() > 11) {
          timestamp.erase(0, 11);
        }
        text = utf8_to_wide(timestamp);
      }
      break;
    case 1:
      text = event.duration_us == 0
                 ? L"—"
                 : utf8_to_wide(technology_log_format_duration(
                       event.duration_us, state.russian));
      break;
    case 2:
      text = utf8_to_wide(event.type);
      break;
    case 3:
      text = utf8_to_wide(
          interpret_technology_log_event(source, event, state.russian)
              .summary);
      break;
    default:
      break;
  }
  wcsncpy_s(display.item.pszText,
            static_cast<std::size_t>(display.item.cchTextMax),
            text.c_str(), _TRUNCATE);
}

}  // namespace

bool TechnologyLogView::register_class(const HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance;
  type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  type.hbrBackground = nullptr;
  return RegisterClassExW(&type) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND TechnologyLogView::create(const HWND parent, const int control_id,
                               const bool russian,
                               const TechnologyLogUiState initial_state) {
  CreateOptions options{
      .russian = russian,
      .ui = initial_state,
  };
  return CreateWindowExW(
      0, kClassName, L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
      0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
      GetModuleHandleW(nullptr), &options);
}

bool TechnologyLogView::open(const HWND window,
                             const std::filesystem::path& path) {
  State* state = state_for(window);
  if (!state) return false;
  state->loaded.reset();
  state->visible_events.clear();
  state->path = path;
  ListView_SetItemCount(state->events, 0);
  SetWindowTextW(
      state->summary,
      state->russian ? L"ТЖ · чтение и индексация…"
                     : L"Technology log · reading and indexing…");
  SetWindowTextW(state->details, L"");
  SetWindowTextW(state->raw, L"");
  state->controller.start(window, kLoadComplete, path);
  return true;
}

bool TechnologyLogView::looks_like(
    const std::filesystem::path& path) {
  MappedFile file;
  if (!file.open(path) || !file.data() ||
      file.size() >
          static_cast<std::uint64_t>(
              (std::numeric_limits<std::size_t>::max)())) {
    return false;
  }
  const std::size_t sample_size = static_cast<std::size_t>((std::min)(
      file.size(), static_cast<std::uint64_t>(64 * 1024)));
  return looks_like_technology_log(
      {reinterpret_cast<const char*>(file.data()), sample_size});
}

void TechnologyLogView::set_dark(const HWND window, const bool dark) {
  SendMessageW(window, kSetDark, dark ? TRUE : FALSE, 0);
}

bool TechnologyLogView::find_next(
    const HWND window, const std::string& pattern,
    const SearchOptions options, const bool wrap) {
  State* state = state_for(window);
  if (!state || !state->loaded || pattern.empty() ||
      state->visible_events.empty()) {
    return false;
  }
  int selected = -1;
  (void)selected_event(*state, &selected);
  const int count = static_cast<int>(state->visible_events.size());
  const int first = selected >= 0 ? selected + 1 : 0;
  const int attempts = wrap ? count : count - first;
  const std::string_view source = source_for(*state);
  for (int offset = 0; offset < attempts; ++offset) {
    const int visible_index = (first + offset) % count;
    const std::size_t event_index =
        state->visible_events[static_cast<std::size_t>(visible_index)];
    const TechnologyLogEvent& event =
        state->loaded->document.events[event_index];
    const SearchOneResult found = search_next(
        technology_log_raw_event(source, event), pattern, options, 0,
        false);
    if (!found.ok || !found.found) continue;
    ListView_SetItemState(
        state->events, visible_index, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state->events, visible_index, FALSE);
    SetFocus(state->events);
    return true;
  }
  MessageBeep(MB_ICONINFORMATION);
  return false;
}

bool TechnologyLogView::copy(const HWND window) {
  State* state = state_for(window);
  if (!state) return false;
  const HWND focus = GetFocus();
  if (focus == state->details || focus == state->raw) {
    SendMessageW(focus, WM_COPY, 0, 0);
    return true;
  }
  const TechnologyLogEvent* event = selected_event(*state);
  if (!event) return false;
  copy_wide_text(display_text(
      technology_log_raw_event(source_for(*state), *event)));
  return true;
}

bool TechnologyLogView::select_all(const HWND window) {
  State* state = state_for(window);
  if (!state) return false;
  const HWND focus = GetFocus();
  if (focus == state->details || focus == state->raw) {
    SendMessageW(focus, EM_SETSEL, 0, -1);
    return true;
  }
  return false;
}

TechnologyLogUiState TechnologyLogView::ui_state(const HWND window) {
  const State* state = state_for(window);
  return state ? state->ui : TechnologyLogUiState{};
}

LRESULT CALLBACK TechnologyLogView::window_proc(
    const HWND window, const UINT message, const WPARAM wparam,
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
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP |
                              SS_CENTERIMAGE,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSummaryId)),
        nullptr, nullptr);
    state->filter = CreateWindowExW(
        0, L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
            WS_VSCROLL,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFilterId)),
        nullptr, nullptr);
    state->events = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEventsId)),
        nullptr, nullptr);
    state->raw_toggle = CreateWindowExW(
        0, L"BUTTON",
        state->russian ? L"Показать исходную запись"
                       : L"Show raw record",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRawToggleId)),
        nullptr, nullptr);
    constexpr DWORD edit_style =
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL |
        ES_NOHIDESEL;
    state->details = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", edit_style,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDetailsId)),
        nullptr, nullptr);
    state->raw = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", edit_style,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRawId)),
        nullptr, nullptr);
    SendMessageW(state->details, EM_SETLIMITTEXT, 0x7ffffffe, 0);
    SendMessageW(state->raw, EM_SETLIMITTEXT, 0x7ffffffe, 0);

    const std::array<const wchar_t*, 5> filters = state->russian
        ? std::array<const wchar_t*, 5>{
              L"Все события", L"Предупреждения", L"Медленные (>1 с)",
              L"HTTP", L"Кэш"}
        : std::array<const wchar_t*, 5>{
              L"All events", L"Warnings", L"Slow (>1 s)", L"HTTP",
              L"Cache"};
    for (const wchar_t* filter : filters) {
      SendMessageW(state->filter, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(filter));
    }
    SendMessageW(
        state->filter, CB_SETCURSEL,
        static_cast<WPARAM>(state->ui.filter), 0);

    ListView_SetExtendedListViewStyle(
        state->events,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const std::array<const wchar_t*, 4> columns = state->russian
        ? std::array<const wchar_t*, 4>{
              L"Время", L"Длительность", L"Событие", L"Что произошло"}
        : std::array<const wchar_t*, 4>{
              L"Time", L"Duration", L"Event", L"What happened"};
    for (int index = 0; index < static_cast<int>(columns.size()); ++index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
      column.pszText = const_cast<wchar_t*>(columns[index]);
      column.iSubItem = index;
      column.cx = 100;
      ListView_InsertColumn(state->events, index, &column);
    }
    Button_SetCheck(
        state->raw_toggle,
        state->ui.show_raw ? BST_CHECKED : BST_UNCHECKED);
    ShowWindow(state->raw, state->ui.show_raw ? SW_SHOW : SW_HIDE);
    apply_theme(window, *state);
    apply_layout(window, *state);
    return 0;
  } else if (message == WM_SIZE && state) {
    apply_layout(window, *state);
    return 0;
  } else if (message == WM_SETFONT && state) {
    state->font = reinterpret_cast<HFONT>(wparam);
    for (const HWND control :
         {state->summary, state->filter, state->events, state->raw_toggle,
          state->details, state->raw}) {
      SendMessageW(control, WM_SETFONT, wparam, TRUE);
    }
    return 0;
  } else if (message == WM_SETFOCUS && state) {
    SetFocus(state->events);
    return 0;
  } else if (message == kSetDark && state) {
    state->dark = wparam != FALSE;
    apply_theme(window, *state);
    return 0;
  } else if (message == kLoadComplete && state) {
    std::unique_ptr<TechnologyLogLoadResult> completed(
        reinterpret_cast<TechnologyLogLoadResult*>(lparam));
    if (!completed || !state->controller.accepts(*completed)) return 0;
    if (completed->error != ERROR_SUCCESS) {
      const std::wstring message_text =
          (state->russian ? L"Не удалось прочитать ТЖ:\r\n"
                          : L"Unable to read the technology log:\r\n") +
          win32_error_message(completed->error);
      SetWindowTextW(state->summary, message_text.c_str());
      return 0;
    }
    state->loaded = std::move(completed);
    if (state->loaded->document.events.empty()) {
      SetWindowTextW(
          state->summary,
          state->russian
              ? L"Записи ТЖ не найдены. Переключитесь в текстовый вид."
              : L"No technology log records found. Switch to text view.");
    } else {
      update_summary(*state);
    }
    rebuild_visible(*state);
    return 0;
  } else if (message == WM_COMMAND && state) {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (id == kFilterId && notification == CBN_SELCHANGE) {
      const LRESULT selected =
          SendMessageW(state->filter, CB_GETCURSEL, 0, 0);
      state->ui.filter =
          selected >= 0 &&
                  selected <=
                      static_cast<LRESULT>(TechnologyLogFilter::Cache)
              ? static_cast<TechnologyLogFilter>(selected)
              : TechnologyLogFilter::All;
      rebuild_visible(*state);
      return 0;
    }
    if (id == kRawToggleId && notification == BN_CLICKED) {
      state->ui.show_raw =
          Button_GetCheck(state->raw_toggle) == BST_CHECKED;
      apply_layout(window, *state);
      return 0;
    }
  } else if (message == WM_NOTIFY && state) {
    const auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header && header->hwndFrom == state->events) {
      if (header->code == LVN_GETDISPINFOW) {
        provide_list_text(
            *state, *reinterpret_cast<NMLVDISPINFOW*>(lparam));
        return 0;
      }
      if (header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((change->uChanged & LVIF_STATE) != 0 &&
            (change->uNewState & LVIS_SELECTED) != 0) {
          update_details(*state);
        }
        return 0;
      }
    }
  } else if ((message == WM_CTLCOLORSTATIC ||
              message == WM_CTLCOLOREDIT) &&
             state) {
    const HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(
        dc, state->dark ? RGB(225, 225, 225)
                        : GetSysColor(COLOR_WINDOWTEXT));
    SetBkColor(
        dc, state->dark ? RGB(30, 30, 30)
                        : GetSysColor(COLOR_WINDOW));
    return reinterpret_cast<LRESULT>(
        state->background_brush
            ? state->background_brush
            : GetSysColorBrush(COLOR_WINDOW));
  } else if (message == WM_ERASEBKGND && state) {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(reinterpret_cast<HDC>(wparam), &client,
             state->background_brush
                 ? state->background_brush
                 : GetSysColorBrush(COLOR_WINDOW));
    return 1;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace listopad::app
