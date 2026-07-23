#include "hex_view_window.h"

#include "listopad/file_io.h"
#include "listopad/hex_view.h"
#include "listopad/strings.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <uxtheme.h>
#include <vector>
#include <windowsx.h>

namespace listopad::app {
namespace {

constexpr wchar_t kClassName[] = L"ListopadPPHexView";
constexpr UINT kSearchComplete = WM_APP + 80;
constexpr UINT kSetDark = WM_APP + 81;
constexpr int kScrollScale = 1'000'000;
constexpr int kPadding = 6;

struct State {
  MappedFile file;
  std::uint64_t first_row{0};
  int horizontal_offset{0};
  std::uint64_t selection_offset{0};
  std::size_t selection_length{0};
  bool has_selection{false};
  HFONT font{nullptr};
  bool dark{false};
  std::mutex result_mutex;
  std::optional<SearchOneResult> search_result;
  std::uint64_t search_generation{0};
  std::jthread search;
};

struct Metrics {
  int character_width{8};
  int line_height{16};
  int visible_rows{1};
  int content_width{0};
  unsigned offset_width{8};
};

State* state_for(const HWND window) {
  return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

Metrics metrics_for(const HWND window, const State& state, HDC supplied = nullptr) {
  HDC dc = supplied ? supplied : GetDC(window);
  HFONT previous = nullptr;
  if (state.font && dc)
    previous = static_cast<HFONT>(SelectObject(dc, state.font));
  TEXTMETRICW text{};
  if (dc) GetTextMetricsW(dc, &text);
  if (previous) SelectObject(dc, previous);
  if (!supplied && dc) ReleaseDC(window, dc);

  RECT client{};
  GetClientRect(window, &client);
  Metrics result;
  result.character_width = std::max(1L, text.tmAveCharWidth);
  result.line_height = std::max(1L, text.tmHeight + text.tmExternalLeading);
  result.visible_rows =
      std::max(1, static_cast<int>(
                      (client.bottom - client.top - 2) / result.line_height));
  result.offset_width = hex_offset_width(state.file.size());
  const int characters = static_cast<int>(result.offset_width) + 2 +
                         static_cast<int>(kHexBytesPerRow * 3 - 1) + 3 +
                         static_cast<int>(kHexBytesPerRow);
  result.content_width = kPadding * 2 + characters * result.character_width;
  return result;
}

std::uint64_t maximum_first_row(const State& state, const Metrics& metrics) {
  const std::uint64_t rows = hex_row_count(state.file.size());
  return rows > static_cast<std::uint64_t>(metrics.visible_rows)
      ? rows - static_cast<std::uint64_t>(metrics.visible_rows)
      : 0;
}

void update_scrollbars(const HWND window, State& state) {
  const Metrics metrics = metrics_for(window, state);
  const std::uint64_t rows = hex_row_count(state.file.size());
  const std::uint64_t maximum = maximum_first_row(state, metrics);
  state.first_row = std::min(state.first_row, maximum);

  SCROLLINFO vertical{sizeof(vertical), SIF_RANGE | SIF_PAGE | SIF_POS};
  vertical.nMin = 0;
  vertical.nMax = kScrollScale;
  vertical.nPage = rows == 0
      ? static_cast<UINT>(kScrollScale + 1)
      : static_cast<UINT>(std::clamp<std::uint64_t>(
            static_cast<std::uint64_t>(metrics.visible_rows) * kScrollScale /
                rows,
            1, kScrollScale + 1));
  const int maximum_position =
      std::max(0, vertical.nMax - static_cast<int>(vertical.nPage) + 1);
  vertical.nPos = maximum == 0
      ? 0
      : static_cast<int>(
            static_cast<long double>(state.first_row) * maximum_position /
            maximum);
  SetScrollInfo(window, SB_VERT, &vertical, TRUE);

  RECT client{};
  GetClientRect(window, &client);
  SCROLLINFO horizontal{sizeof(horizontal), SIF_RANGE | SIF_PAGE | SIF_POS};
  horizontal.nMin = 0;
  horizontal.nMax = std::max(0, metrics.content_width - 1);
  horizontal.nPage = static_cast<UINT>(std::max(0L, client.right - client.left));
  const int horizontal_maximum =
      std::max(0, horizontal.nMax - static_cast<int>(horizontal.nPage) + 1);
  state.horizontal_offset =
      std::clamp(state.horizontal_offset, 0, horizontal_maximum);
  horizontal.nPos = state.horizontal_offset;
  SetScrollInfo(window, SB_HORZ, &horizontal, TRUE);
}

void set_first_row(const HWND window, State& state, const std::uint64_t row) {
  const Metrics metrics = metrics_for(window, state);
  state.first_row = std::min(row, maximum_first_row(state, metrics));
  update_scrollbars(window, state);
  InvalidateRect(window, nullptr, TRUE);
}

std::wstring widen_ascii(const std::string_view text) {
  return std::wstring(text.begin(), text.end());
}

void draw_selected_text(HDC dc, const int x, const int y,
                        const int character_width, const std::string_view text,
                        const std::size_t first, const std::size_t count,
                        const COLORREF selected_text) {
  if (count == 0 || first >= text.size()) return;
  const std::string_view selected =
      text.substr(first, std::min(count, text.size() - first));
  const std::wstring wide = widen_ascii(selected);
  const COLORREF previous = SetTextColor(dc, selected_text);
  TextOutW(dc, x + static_cast<int>(first) * character_width, y,
           wide.data(), static_cast<int>(wide.size()));
  SetTextColor(dc, previous);
}

}  // namespace

bool HexViewWindow::register_class(const HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance;
  type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  return RegisterClassExW(&type) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND HexViewWindow::create(const HWND parent, const int control_id) {
  return CreateWindowExW(
      0, kClassName, L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL,
      0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
      GetModuleHandleW(nullptr), nullptr);
}

bool HexViewWindow::open(const HWND window,
                         const std::filesystem::path& path) {
  State* state = state_for(window);
  if (!state) return false;
  state->search.request_stop();
  state->search = std::jthread{};
  {
    std::scoped_lock lock(state->result_mutex);
    state->search_result.reset();
  }
  if (!state->file.open(path)) return false;
  state->first_row = 0;
  state->horizontal_offset = 0;
  state->has_selection = false;
  update_scrollbars(window, *state);
  InvalidateRect(window, nullptr, TRUE);
  return true;
}

void HexViewWindow::set_dark(const HWND window, const bool dark) {
  SetWindowTheme(window, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
  SendMessageW(window, kSetDark, dark ? TRUE : FALSE, 0);
}

bool HexViewWindow::find_next(const HWND window, std::string pattern,
                              const SearchOptions options, const bool wrap) {
  State* state = state_for(window);
  if (!state || state->file.size() == 0 || pattern.empty() ||
      state->file.size() > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  state->search.request_stop();
  state->search = std::jthread{};
  const std::uint64_t generation = ++state->search_generation;
  const std::size_t start = state->has_selection
      ? static_cast<std::size_t>(std::min<std::uint64_t>(
            state->selection_offset +
                std::max<std::size_t>(state->selection_length, 1),
            state->file.size()))
      : static_cast<std::size_t>(std::min<std::uint64_t>(
            state->first_row * kHexBytesPerRow, state->file.size()));
  std::vector<std::byte> query;
  bool case_insensitive = false;
  if (const auto parsed = parse_hex_pattern(pattern)) {
    query = *parsed;
  } else {
    query.assign(reinterpret_cast<const std::byte*>(pattern.data()),
                 reinterpret_cast<const std::byte*>(pattern.data() +
                                                     pattern.size()));
    case_insensitive = !options.match_case;
  }
  const std::span<const std::byte> subject(
      state->file.data(), static_cast<std::size_t>(state->file.size()));
  state->search = std::jthread(
      [state, window, generation, subject, start, query = std::move(query),
       case_insensitive, wrap](const std::stop_token stop) {
        SearchOneResult result =
            find_next_bytes(subject, query, start, wrap, case_insensitive, stop);
        if (stop.stop_requested()) return;
        {
          std::scoped_lock lock(state->result_mutex);
          if (generation != state->search_generation) return;
          state->search_result = std::move(result);
        }
        PostMessageW(window, kSearchComplete, 0, 0);
      });
  return true;
}

LRESULT CALLBACK HexViewWindow::window_proc(
    const HWND window, const UINT message, const WPARAM wparam,
    const LPARAM lparam) {
  State* state = state_for(window);
  if (message == WM_NCCREATE) {
    state = new State;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
  } else if (message == WM_NCDESTROY) {
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
  } else if (message == WM_SETFONT && state) {
    state->font = reinterpret_cast<HFONT>(wparam);
    update_scrollbars(window, *state);
    InvalidateRect(window, nullptr, TRUE);
  } else if (message == kSetDark && state) {
    state->dark = wparam != FALSE;
    InvalidateRect(window, nullptr, TRUE);
    return 0;
  } else if (message == WM_SIZE && state) {
    update_scrollbars(window, *state);
    return 0;
  } else if (message == WM_ERASEBKGND && state) {
    RECT client{};
    GetClientRect(window, &client);
    SetDCBrushColor(reinterpret_cast<HDC>(wparam),
                    state->dark ? RGB(30, 30, 30)
                                : GetSysColor(COLOR_WINDOW));
    FillRect(reinterpret_cast<HDC>(wparam), &client,
             static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    return 1;
  } else if (message == WM_VSCROLL && state) {
    const Metrics metrics = metrics_for(window, *state);
    const std::uint64_t maximum = maximum_first_row(*state, metrics);
    std::uint64_t row = state->first_row;
    switch (LOWORD(wparam)) {
      case SB_LINEUP:
        if (row > 0) --row;
        break;
      case SB_LINEDOWN:
        ++row;
        break;
      case SB_PAGEUP:
        row = row > static_cast<std::uint64_t>(metrics.visible_rows - 1)
            ? row - static_cast<std::uint64_t>(metrics.visible_rows - 1)
            : 0;
        break;
      case SB_PAGEDOWN:
        row += static_cast<std::uint64_t>(metrics.visible_rows - 1);
        break;
      case SB_TOP:
        row = 0;
        break;
      case SB_BOTTOM:
        row = maximum;
        break;
      case SB_THUMBPOSITION:
      case SB_THUMBTRACK: {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS | SIF_PAGE | SIF_RANGE};
        GetScrollInfo(window, SB_VERT, &info);
        const int maximum_position =
            std::max(1, info.nMax - static_cast<int>(info.nPage) + 1);
        row = static_cast<std::uint64_t>(
            static_cast<long double>(std::max(0, info.nTrackPos)) * maximum /
            maximum_position);
        break;
      }
      default:
        return 0;
    }
    set_first_row(window, *state, std::min(row, maximum));
    return 0;
  } else if (message == WM_HSCROLL && state) {
    SCROLLINFO info{sizeof(info), SIF_ALL};
    GetScrollInfo(window, SB_HORZ, &info);
    int position = state->horizontal_offset;
    switch (LOWORD(wparam)) {
      case SB_LINELEFT: position -= 16; break;
      case SB_LINERIGHT: position += 16; break;
      case SB_PAGELEFT: position -= static_cast<int>(info.nPage); break;
      case SB_PAGERIGHT: position += static_cast<int>(info.nPage); break;
      case SB_LEFT: position = 0; break;
      case SB_RIGHT: position = info.nMax; break;
      case SB_THUMBPOSITION:
      case SB_THUMBTRACK: position = info.nTrackPos; break;
      default: return 0;
    }
    const int maximum =
        std::max(0, info.nMax - static_cast<int>(info.nPage) + 1);
    state->horizontal_offset = std::clamp(position, 0, maximum);
    update_scrollbars(window, *state);
    InvalidateRect(window, nullptr, TRUE);
    return 0;
  } else if (message == WM_MOUSEWHEEL && state) {
    const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
    std::uint64_t row = state->first_row;
    const std::uint64_t amount =
        static_cast<std::uint64_t>(std::abs(notches) * 3);
    if (notches > 0)
      row = row > amount ? row - amount : 0;
    else
      row += amount;
    set_first_row(window, *state, row);
    return 0;
  } else if (message == WM_KEYDOWN && state) {
    switch (wparam) {
      case VK_UP: SendMessageW(window, WM_VSCROLL, SB_LINEUP, 0); return 0;
      case VK_DOWN: SendMessageW(window, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
      case VK_PRIOR: SendMessageW(window, WM_VSCROLL, SB_PAGEUP, 0); return 0;
      case VK_NEXT: SendMessageW(window, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
      case VK_HOME: SendMessageW(window, WM_VSCROLL, SB_TOP, 0); return 0;
      case VK_END: SendMessageW(window, WM_VSCROLL, SB_BOTTOM, 0); return 0;
      default: break;
    }
  } else if (message == kSearchComplete && state) {
    std::optional<SearchOneResult> result;
    {
      std::scoped_lock lock(state->result_mutex);
      result.swap(state->search_result);
    }
    if (!result) return 0;
    if (!result->ok) {
      if (result->error != "Search cancelled") {
        MessageBoxW(GetParent(window), utf8_to_wide(result->error).c_str(),
                    L"Listopad++", MB_OK | MB_ICONERROR);
      }
      return 0;
    }
    if (!result->found) {
      MessageBeep(MB_ICONINFORMATION);
      return 0;
    }
    state->selection_offset = result->match.start;
    state->selection_length = result->match.length;
    state->has_selection = true;
    const Metrics metrics = metrics_for(window, *state);
    const std::uint64_t selected_row =
        state->selection_offset / kHexBytesPerRow;
    const std::uint64_t half =
        static_cast<std::uint64_t>(metrics.visible_rows / 2);
    set_first_row(window, *state,
                  selected_row > half ? selected_row - half : 0);
    return 0;
  } else if (message == WM_PAINT && state) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const COLORREF background =
        state->dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW);
    const COLORREF foreground =
        state->dark ? RGB(220, 220, 220) : GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF selection =
        state->dark ? RGB(62, 95, 135) : GetSysColor(COLOR_HIGHLIGHT);
    const COLORREF selection_text =
        state->dark ? RGB(255, 255, 255)
                    : GetSysColor(COLOR_HIGHLIGHTTEXT);
    SetDCBrushColor(dc, background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    HFONT old_font =
        state->font ? static_cast<HFONT>(SelectObject(dc, state->font)) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground);
    const Metrics metrics = metrics_for(window, *state, dc);
    const int offset_x = kPadding - state->horizontal_offset;
    const int hex_x =
        offset_x + (static_cast<int>(metrics.offset_width) + 2) *
                       metrics.character_width;
    const int ascii_x =
        hex_x + (static_cast<int>(kHexBytesPerRow * 3 - 1) + 3) *
                    metrics.character_width;
    const std::uint64_t rows = hex_row_count(state->file.size());
    for (int visible = 0; visible < metrics.visible_rows; ++visible) {
      const std::uint64_t row_index =
          state->first_row + static_cast<std::uint64_t>(visible);
      if (row_index >= rows) break;
      const std::uint64_t offset = row_index * kHexBytesPerRow;
      const std::size_t length = static_cast<std::size_t>(
          std::min<std::uint64_t>(kHexBytesPerRow,
                                  state->file.size() - offset));
      const std::span<const std::byte> bytes(
          state->file.data() + static_cast<std::size_t>(offset), length);
      const HexRowText row =
          format_hex_row(bytes, offset, metrics.offset_width);
      const int y = 2 + visible * metrics.line_height;

      std::size_t selected_first = 0;
      std::size_t selected_count = 0;
      if (state->has_selection) {
        const std::uint64_t selection_end =
            state->selection_offset + state->selection_length;
        const std::uint64_t row_end = offset + length;
        const std::uint64_t first =
            std::max(offset, state->selection_offset);
        const std::uint64_t end = std::min(row_end, selection_end);
        if (end > first) {
          selected_first = static_cast<std::size_t>(first - offset);
          selected_count = static_cast<std::size_t>(end - first);
          SetDCBrushColor(dc, selection);
          RECT hex_selection{
              hex_x + static_cast<int>(selected_first * 3) *
                          metrics.character_width,
              y,
              hex_x + static_cast<int>(
                          selected_first * 3 + selected_count * 3 - 1) *
                          metrics.character_width,
              y + metrics.line_height};
          RECT ascii_selection{
              ascii_x + static_cast<int>(selected_first) *
                            metrics.character_width,
              y,
              ascii_x + static_cast<int>(selected_first + selected_count) *
                            metrics.character_width,
              y + metrics.line_height};
          FillRect(dc, &hex_selection,
                   static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
          FillRect(dc, &ascii_selection,
                   static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
      }

      const std::wstring offset_text = widen_ascii(row.offset);
      const std::wstring hex_text = widen_ascii(row.hex);
      const std::wstring ascii_text = widen_ascii(row.ascii);
      TextOutW(dc, offset_x, y, offset_text.data(),
               static_cast<int>(offset_text.size()));
      TextOutW(dc, hex_x, y, hex_text.data(),
               static_cast<int>(hex_text.size()));
      TextOutW(dc, ascii_x, y, ascii_text.data(),
               static_cast<int>(ascii_text.size()));
      if (selected_count > 0) {
        draw_selected_text(dc, hex_x, y, metrics.character_width, row.hex,
                           selected_first * 3, selected_count * 3 - 1,
                           selection_text);
        draw_selected_text(dc, ascii_x, y, metrics.character_width, row.ascii,
                           selected_first, selected_count, selection_text);
      }
    }
    if (old_font) SelectObject(dc, old_font);
    EndPaint(window, &paint);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace listopad::app
