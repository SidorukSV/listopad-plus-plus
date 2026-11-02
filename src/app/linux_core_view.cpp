#include "linux_core_view.h"

#include "linux_core_controller.h"
#include "listopad/strings.h"

#include <windows.h>
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

namespace listopad::app {
namespace {

constexpr wchar_t kClassName[] = L"ListopadPPLinuxCore";
constexpr UINT kLoadComplete = WM_APP + 104;
constexpr UINT kSetDark = WM_APP + 105;
constexpr int kSummaryId = 1;
constexpr int kMappingsId = 2;
constexpr int kColumnCount = 4;

struct CreateOptions {
  bool russian{true};
  LinuxCoreUiState ui;
};

struct State {
  LinuxCoreController controller;
  LinuxCoreDocument document;
  HWND summary{nullptr};
  HWND mappings{nullptr};
  HBRUSH background_brush{nullptr};
  bool russian{true};
  bool dark{false};
  bool loaded{false};
  std::wstring status;
  LinuxCoreUiState ui;

  ~State() {
    if (background_brush) DeleteObject(background_brush);
  }
};

State* state_for(const HWND window) noexcept {
  return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

LRESULT CALLBACK header_subclass_proc(const HWND window, const UINT message,
                                      const WPARAM wparam,
                                      const LPARAM lparam, const UINT_PTR,
                                      const DWORD_PTR reference) {
  const auto* state = reinterpret_cast<const State*>(reference);
  if (!state || !state->dark) {
    return DefSubclassProc(window, message, wparam, lparam);
  }
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_THEMECHANGED) {
    InvalidateRect(window, nullptr, TRUE);
  } else if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    const HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const HBRUSH background = CreateSolidBrush(RGB(45, 45, 45));
    FillRect(dc, &client, background);
    DeleteObject(background);

    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(window, WM_GETFONT, 0, 0));
    const HGDIOBJ previous_font = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(225, 225, 225));
    const HPEN separator = CreatePen(PS_SOLID, 1, RGB(70, 70, 70));
    const HGDIOBJ previous_pen = SelectObject(dc, separator);
    const int padding = MulDiv(6, static_cast<int>(GetDpiForWindow(window)),
                               96);
    const int item_count = Header_GetItemCount(window);
    for (int index = 0; index < item_count; ++index) {
      RECT item{};
      if (!Header_GetItemRect(window, index, &item)) continue;
      std::array<wchar_t, 256> text{};
      HDITEMW details{};
      details.mask = HDI_TEXT;
      details.pszText = text.data();
      details.cchTextMax = static_cast<int>(text.size());
      Header_GetItem(window, index, &details);
      RECT label = item;
      label.left += padding;
      label.right -= padding;
      DrawTextW(dc, text.data(), -1, &label,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
      MoveToEx(dc, item.right - 1, item.top, nullptr);
      LineTo(dc, item.right - 1, item.bottom);
    }
    SelectObject(dc, previous_pen);
    DeleteObject(separator);
    if (previous_font) SelectObject(dc, previous_font);
    EndPaint(window, &paint);
    return 0;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

std::wstring hex_value(const std::uint64_t value) {
  std::array<wchar_t, 32> buffer{};
  swprintf_s(buffer.data(), buffer.size(), L"0x%llX",
             static_cast<unsigned long long>(value));
  return buffer.data();
}

std::wstring timestamp_text(const LinuxCoreSourceMetadata& source,
                            const bool russian) {
  if (!source.has_timestamp) return russian ? L"неизвестно" : L"unknown";
  constexpr std::uint64_t kUnixToFileTime = 116444736000000000ull;
  if (source.timestamp_microseconds >
      (std::numeric_limits<std::uint64_t>::max() - kUnixToFileTime) / 10) {
    return russian ? L"неизвестно" : L"unknown";
  }
  const std::uint64_t ticks =
      kUnixToFileTime + source.timestamp_microseconds * 10;
  FILETIME file_time{static_cast<DWORD>(ticks & 0xffffffffu),
                     static_cast<DWORD>(ticks >> 32)};
  SYSTEMTIME utc{};
  if (!FileTimeToSystemTime(&file_time, &utc)) {
    return russian ? L"неизвестно" : L"unknown";
  }
  std::array<wchar_t, 64> buffer{};
  swprintf_s(buffer.data(), buffer.size(), L"%04u-%02u-%02u %02u:%02u:%02u UTC",
             utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute,
             utc.wSecond);
  return buffer.data();
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

std::wstring error_text(const LinuxCoreReadResult& parsed,
                        const bool russian) {
  if (parsed.system_error != 0) {
    return (russian ? L"Не удалось открыть дамп: "
                    : L"Unable to open the core dump: ") +
           win32_error_message(parsed.system_error);
  }
  switch (parsed.error) {
    case LinuxCoreError::NotCoreDump:
      return russian ? L"Файл не является ELF core dump."
                     : L"The file is not an ELF core dump.";
    case LinuxCoreError::UnsupportedFormat:
      return russian ? L"Формат или архитектура ELF пока не поддерживается."
                     : L"The ELF format or architecture is not supported yet.";
    case LinuxCoreError::MetadataTooLarge:
      return russian ? L"Служебные данные дампа превышают безопасный лимит."
                     : L"Core metadata exceeds the safety limit.";
    case LinuxCoreError::DecompressionFailed:
      return russian ? L"Не удалось распаковать поток Zstandard."
                     : L"Unable to decompress the Zstandard stream.";
    case LinuxCoreError::InvalidData:
      return russian ? L"Дамп повреждён или обрезан."
                     : L"The core dump is damaged or truncated.";
    case LinuxCoreError::OpenFailed:
      return russian ? L"Не удалось открыть дамп."
                     : L"Unable to open the core dump.";
    case LinuxCoreError::Cancelled:
      return russian ? L"Чтение отменено." : L"Reading was cancelled.";
    case LinuxCoreError::None:
      return {};
  }
  return russian ? L"Не удалось прочитать дамп."
                 : L"Unable to read the core dump.";
}

void update_summary(State& state) {
  if (!state.loaded) {
    SetWindowTextW(state.summary, state.status.c_str());
    return;
  }
  const LinuxCoreSourceMetadata& source = state.document.source;
  std::wstring text = state.russian ? L"Дамп Linux" : L"Linux core dump";
  if (!source.process.empty()) {
    text += state.russian ? L" · процесс: " : L" · process: ";
    text += utf8_to_wide(source.process);
  }
  if (source.has_pid) {
    text += L" · PID: " + std::to_wstring(source.pid);
  }
  text += L" · " + utf8_to_wide(linux_core_architecture_name(
                       state.document.architecture));
  text += state.document.elf_class == 1 ? L" (ELF32)" : L" (ELF64)";
  text += L"\r\n";
  text += state.russian ? L"Сигнал: " : L"Signal: ";
  text += utf8_to_wide(linux_signal_name(state.document.signal));
  text += L" (" + std::to_wstring(state.document.signal) + L")";
  text += state.russian ? L" · потоков: " : L" · threads: ";
  text += std::to_wstring(state.document.thread_count);
  text += state.russian ? L" · сегментов памяти: " : L" · memory segments: ";
  text += std::to_wstring(state.document.load_segment_count);
  text += state.russian ? L" · отображений файлов: " : L" · file mappings: ";
  text += std::to_wstring(state.document.mappings.size());
  if (state.document.mappings_truncated) {
    text += state.russian ? L" (список усечён)" : L" (list truncated)";
  }
  text += L"\r\n";
  text += state.russian ? L"Время: " : L"Time: ";
  text += timestamp_text(source, state.russian);
  if (!source.boot_id.empty()) {
    text += L" · boot ID: " + utf8_to_wide(source.boot_id);
  }
  SetWindowTextW(state.summary, text.c_str());
}

void apply_layout(const HWND window, State& state) {
  RECT client{};
  GetClientRect(window, &client);
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const LinuxCoreLayout layout = calculate_linux_core_layout(
      client.right - client.left, client.bottom - client.top, dpi);
  MoveWindow(state.summary, layout.summary.x, layout.summary.y,
             layout.summary.width, layout.summary.height, TRUE);
  MoveWindow(state.mappings, layout.mappings.x, layout.mappings.y,
             layout.mappings.width, layout.mappings.height, TRUE);
  const std::array<int, kColumnCount> widths{
      MulDiv(145, dpi, 96), MulDiv(145, dpi, 96),
      MulDiv(145, dpi, 96), MulDiv(540, dpi, 96)};
  for (int index = 0; index < kColumnCount; ++index) {
    ListView_SetColumnWidth(state.mappings, index, widths[index]);
  }
}

void apply_theme(const HWND window, State& state) {
  if (state.background_brush) DeleteObject(state.background_brush);
  const COLORREF background =
      state.dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW);
  const COLORREF foreground =
      state.dark ? RGB(225, 225, 225) : GetSysColor(COLOR_WINDOWTEXT);
  state.background_brush = CreateSolidBrush(background);
  ListView_SetBkColor(state.mappings, background);
  ListView_SetTextBkColor(state.mappings, background);
  ListView_SetTextColor(state.mappings, foreground);
  SetWindowTheme(state.mappings,
                 state.dark ? L"DarkMode_Explorer" : nullptr, nullptr);
  if (const HWND header = ListView_GetHeader(state.mappings)) {
    SetWindowTheme(header, state.dark ? L"DarkMode_Explorer" : nullptr,
                   nullptr);
    SendMessageW(header, WM_THEMECHANGED, 0, 0);
    InvalidateRect(header, nullptr, TRUE);
  }
  InvalidateRect(window, nullptr, TRUE);
}

std::wstring cell_text(const LinuxCoreMapping& mapping, const int column) {
  switch (column) {
    case 0: return hex_value(mapping.start);
    case 1: return hex_value(mapping.end);
    case 2: return hex_value(mapping.file_offset);
    case 3: return utf8_to_wide(mapping.path);
    default: return {};
  }
}

void provide_text(const State& state, NMLVDISPINFOW& display) {
  if ((display.item.mask & LVIF_TEXT) == 0 || display.item.iItem < 0 ||
      static_cast<std::size_t>(display.item.iItem) >=
          state.document.mappings.size() ||
      !display.item.pszText || display.item.cchTextMax <= 0) {
    return;
  }
  const std::wstring text = cell_text(
      state.document.mappings[static_cast<std::size_t>(display.item.iItem)],
      display.item.iSubItem);
  wcsncpy_s(display.item.pszText,
            static_cast<std::size_t>(display.item.cchTextMax), text.c_str(),
            _TRUNCATE);
}

std::wstring mapping_as_text(const State& state,
                             const LinuxCoreMapping& mapping) {
  std::wstring result =
      state.russian ? L"Начало\tКонец\tСмещение файла\tПуть\r\n"
                    : L"Start\tEnd\tFile offset\tPath\r\n";
  for (int column = 0; column < kColumnCount; ++column) {
    if (column != 0) result.push_back(L'\t');
    result += cell_text(mapping, column);
  }
  result += L"\r\n";
  return result;
}

}  // namespace

bool LinuxCoreView::register_class(const HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance;
  type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  type.hbrBackground = nullptr;
  return RegisterClassExW(&type) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND LinuxCoreView::create(const HWND parent, const int control_id,
                           const bool russian,
                           const LinuxCoreUiState initial_state) {
  CreateOptions options{.russian = russian, .ui = initial_state};
  return CreateWindowExW(
      0, kClassName, L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
      0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
      GetModuleHandleW(nullptr), &options);
}

bool LinuxCoreView::open(const HWND window,
                         const std::filesystem::path& path) {
  State* state = state_for(window);
  if (!state) return false;
  state->loaded = false;
  state->document = {};
  state->status = state->russian ? L"Дамп Linux · чтение…"
                                 : L"Linux core dump · reading…";
  ListView_SetItemCount(state->mappings, 0);
  update_summary(*state);
  state->controller.start(window, kLoadComplete, path);
  return true;
}

bool LinuxCoreView::looks_like(const std::filesystem::path& path) {
  return looks_like_linux_core_file(path);
}

void LinuxCoreView::set_dark(const HWND window, const bool dark) {
  SendMessageW(window, kSetDark, dark ? TRUE : FALSE, 0);
}

bool LinuxCoreView::find_next(const HWND window, const std::string& pattern,
                              SearchOptions options, const bool wrap) {
  State* state = state_for(window);
  if (!state || !state->loaded || pattern.empty() ||
      state->document.mappings.empty()) {
    return false;
  }
  options.regular_expression = false;
  int selected = ListView_GetNextItem(state->mappings, -1, LVNI_SELECTED);
  const int count = static_cast<int>(state->document.mappings.size());
  const int first = selected >= 0 ? selected + 1 : 0;
  const int attempts = wrap ? count : count - first;
  for (int offset = 0; offset < attempts; ++offset) {
    const int row = (first + offset) % count;
    const LinuxCoreMapping& mapping =
        state->document.mappings[static_cast<std::size_t>(row)];
    const SearchOneResult found =
        search_next(mapping.path, pattern, options, 0, false);
    if (!found.ok || !found.found) continue;
    ListView_SetItemState(state->mappings, row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state->mappings, row, FALSE);
    SetFocus(state->mappings);
    return true;
  }
  MessageBeep(MB_ICONINFORMATION);
  return false;
}

bool LinuxCoreView::copy(const HWND window) {
  State* state = state_for(window);
  if (!state || !state->loaded) return false;
  const int selected =
      ListView_GetNextItem(state->mappings, -1, LVNI_SELECTED);
  if (selected < 0 || static_cast<std::size_t>(selected) >=
                          state->document.mappings.size()) {
    return false;
  }
  copy_wide_text(mapping_as_text(
      *state, state->document.mappings[static_cast<std::size_t>(selected)]));
  return true;
}

bool LinuxCoreView::select_all(const HWND) {
  return false;
}

LinuxCoreUiState LinuxCoreView::ui_state(const HWND window) {
  const State* state = state_for(window);
  return state ? state->ui : LinuxCoreUiState{};
}

LRESULT CALLBACK LinuxCoreView::window_proc(const HWND window,
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
        0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
        window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSummaryId)),
        nullptr, nullptr);
    state->mappings = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMappingsId)), nullptr,
        nullptr);
    ListView_SetExtendedListViewStyle(
        state->mappings, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                             LVS_EX_LABELTIP | LVS_EX_HEADERDRAGDROP);
    const std::array<const wchar_t*, kColumnCount> columns =
        state->russian
            ? std::array<const wchar_t*, kColumnCount>{
                  L"Начало", L"Конец", L"Смещение файла", L"Путь"}
            : std::array<const wchar_t*, kColumnCount>{
                  L"Start", L"End", L"File offset", L"Path"};
    for (int index = 0; index < kColumnCount; ++index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
      column.pszText = const_cast<wchar_t*>(columns[index]);
      column.iSubItem = index;
      column.cx = 100;
      ListView_InsertColumn(state->mappings, index, &column);
    }
    if (const HWND header = ListView_GetHeader(state->mappings)) {
      SetWindowSubclass(header, header_subclass_proc, 1,
                        reinterpret_cast<DWORD_PTR>(state));
    }
    apply_theme(window, *state);
    apply_layout(window, *state);
    return 0;
  } else if (message == WM_SIZE && state) {
    apply_layout(window, *state);
    return 0;
  } else if (message == WM_SETFONT && state) {
    SendMessageW(state->summary, WM_SETFONT, wparam, TRUE);
    SendMessageW(state->mappings, WM_SETFONT, wparam, TRUE);
    return 0;
  } else if (message == WM_SETFOCUS && state) {
    SetFocus(state->mappings);
    return 0;
  } else if (message == kSetDark && state) {
    state->dark = wparam != FALSE;
    apply_theme(window, *state);
    return 0;
  } else if (message == kLoadComplete && state) {
    std::unique_ptr<LinuxCoreLoadResult> completed(
        reinterpret_cast<LinuxCoreLoadResult*>(lparam));
    if (!completed || !state->controller.accepts(*completed)) return 0;
    if (!completed->parsed.ok()) {
      state->status = error_text(completed->parsed, state->russian);
      update_summary(*state);
      return 0;
    }
    state->document = std::move(completed->parsed.document);
    state->loaded = true;
    update_summary(*state);
    ListView_SetItemCountEx(
        state->mappings,
        static_cast<int>((std::min)(
            state->document.mappings.size(),
            static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
        LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    if (!state->document.mappings.empty()) {
      const int row = static_cast<int>((std::min)(
          state->ui.selected_mapping, state->document.mappings.size() - 1));
      state->ui.selected_mapping = static_cast<std::size_t>(row);
      ListView_SetItemState(state->mappings, row,
                            LVIS_SELECTED | LVIS_FOCUSED,
                            LVIS_SELECTED | LVIS_FOCUSED);
      ListView_EnsureVisible(state->mappings, row, FALSE);
    }
    return 0;
  } else if (message == WM_NOTIFY && state) {
    const auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header && header->hwndFrom == state->mappings) {
      if (header->code == LVN_GETDISPINFOW) {
        provide_text(*state, *reinterpret_cast<NMLVDISPINFOW*>(lparam));
        return 0;
      }
      if (header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((change->uChanged & LVIF_STATE) != 0 &&
            (change->uNewState & LVIS_SELECTED) != 0 &&
            change->iItem >= 0) {
          state->ui.selected_mapping =
              static_cast<std::size_t>(change->iItem);
        }
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
