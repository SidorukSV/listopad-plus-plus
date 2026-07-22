#include "large_file_view.h"

#include "listopad/strings.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <uxtheme.h>

namespace listopad::app {
namespace {

constexpr wchar_t kClassName[] = L"ListopadPPLargeFile";
constexpr UINT kSearchComplete = WM_APP + 70;
constexpr UINT kSetDark = WM_APP + 71;
struct State {
  MappedFile file;
  std::uint64_t offset{0};
  std::uint64_t selection_offset{0};
  std::size_t selection_length{0};
  bool has_selection{false};
  HFONT font{nullptr};
  std::mutex result_mutex;
  std::optional<SearchOneResult> search_result;
  std::uint64_t search_generation{0};
  std::jthread search;
  bool dark{false};
};

std::uint64_t line_start(const MappedFile& file, std::uint64_t offset) {
  if (!file.data() || file.size() == 0) return 0;
  offset = std::min(offset, file.size() - 1);
  while (offset > 0 && file.data()[offset - 1] != std::byte{'\n'}) --offset;
  return offset;
}

}  // namespace

bool LargeFileView::register_class(HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance; type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc; type.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
  type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  return RegisterClassExW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND LargeFileView::create(HWND parent, int control_id) {
  return CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                         WS_VSCROLL | WS_HSCROLL, 0, 0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
                         GetModuleHandleW(nullptr), nullptr);
}

bool LargeFileView::open(HWND window, const std::filesystem::path& path) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state) return false;
  state->search.request_stop();
  state->search = std::jthread{};
  if (!state->file.open(path)) return false;
  state->offset = 0;
  state->has_selection = false;
  SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
  info.nMin = 0; info.nMax = 1'000'000; info.nPage = 1000; info.nPos = 0;
  SetScrollInfo(window, SB_VERT, &info, TRUE);
  InvalidateRect(window, nullptr, TRUE);
  return true;
}

void LargeFileView::set_dark(HWND window, const bool dark) {
  SetWindowTheme(window, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
  SendMessageW(window, kSetDark, dark ? TRUE : FALSE, 0);
}

bool LargeFileView::find_next(HWND window, std::string pattern,
                              SearchOptions options, const bool wrap) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (!state || !state->file.data() || pattern.empty()) return false;
  state->search.request_stop();
  const std::uint64_t generation = ++state->search_generation;
  const std::size_t start = state->has_selection
      ? static_cast<std::size_t>(std::min<std::uint64_t>(
            state->selection_offset + std::max<std::size_t>(state->selection_length, 1),
            state->file.size()))
      : static_cast<std::size_t>(state->offset);
  const std::string_view subject(reinterpret_cast<const char*>(state->file.data()),
                                 static_cast<std::size_t>(state->file.size()));
  state->search = std::jthread(
      [state, window, generation, subject, start, pattern = std::move(pattern), options, wrap]
      (const std::stop_token stop) {
        SearchOneResult result = search_next(subject, pattern, options, start, wrap, stop);
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

LRESULT CALLBACK LargeFileView::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    state = new State;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  } else if (message == WM_NCDESTROY) {
    delete state; SetWindowLongPtrW(window, GWLP_USERDATA, 0);
  } else if (message == WM_SETFONT && state) {
    state->font = reinterpret_cast<HFONT>(wparam); InvalidateRect(window, nullptr, TRUE);
  } else if (message == kSetDark && state) {
    state->dark = wparam != FALSE;
    InvalidateRect(window, nullptr, TRUE);
    return 0;
  } else if (message == WM_ERASEBKGND && state) {
    RECT client{}; GetClientRect(window, &client);
    const COLORREF background = state->dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW);
    SetDCBrushColor(reinterpret_cast<HDC>(wparam), background);
    FillRect(reinterpret_cast<HDC>(wparam), &client,
             static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    return 1;
  } else if (message == WM_VSCROLL && state && state->file.size()) {
    int position = GetScrollPos(window, SB_VERT);
    switch (LOWORD(wparam)) {
      case SB_LINEUP: position -= 1; break; case SB_LINEDOWN: position += 1; break;
      case SB_PAGEUP: position -= 1000; break; case SB_PAGEDOWN: position += 1000; break;
      case SB_THUMBTRACK: position = HIWORD(wparam); break; default: break;
    }
    position = std::clamp(position, 0, 1'000'000);
    SetScrollPos(window, SB_VERT, position, TRUE);
    state->offset = line_start(state->file,
        static_cast<std::uint64_t>((static_cast<long double>(position) / 1'000'000.0L) * state->file.size()));
    InvalidateRect(window, nullptr, TRUE);
    return 0;
  } else if (message == WM_MOUSEWHEEL) {
    SendMessageW(window, WM_VSCROLL, MAKEWPARAM(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
    return 0;
  } else if (message == kSearchComplete && state) {
    std::optional<SearchOneResult> result;
    {
      std::scoped_lock lock(state->result_mutex);
      result.swap(state->search_result);
    }
    if (!result) return 0;
    if (!result->ok) {
      if (result->error != "Search cancelled")
        MessageBoxW(GetParent(window), utf8_to_wide(result->error).c_str(),
                    L"Listopad++", MB_OK | MB_ICONERROR);
      return 0;
    }
    if (!result->found) { MessageBeep(MB_ICONINFORMATION); return 0; }
    state->selection_offset = result->match.start;
    state->selection_length = result->match.length;
    state->has_selection = true;
    state->offset = line_start(state->file, state->selection_offset);
    const int position = state->file.size()
        ? static_cast<int>((static_cast<long double>(state->offset) * 1'000'000.0L) /
                           state->file.size())
        : 0;
    SetScrollPos(window, SB_VERT, std::clamp(position, 0, 1'000'000), TRUE);
    InvalidateRect(window, nullptr, TRUE);
    return 0;
  } else if (message == WM_PAINT && state) {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
    RECT client{}; GetClientRect(window, &client);
    const COLORREF background = state->dark ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW);
    const COLORREF foreground = state->dark ? RGB(220, 220, 220) : GetSysColor(COLOR_WINDOWTEXT);
    SetDCBrushColor(dc, background);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    HFONT old_font = state->font ? static_cast<HFONT>(SelectObject(dc, state->font)) : nullptr;
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, foreground);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    const int height = std::max(1L, metrics.tmHeight + metrics.tmExternalLeading);
    std::uint64_t offset = state->offset;
    for (int y = 2; y < client.bottom && offset < state->file.size(); y += height) {
      std::uint64_t end = offset;
      while (end < state->file.size() && state->file.data()[end] != std::byte{'\n'} && end - offset < 16384) ++end;
      std::string_view line(reinterpret_cast<const char*>(state->file.data() + offset), static_cast<std::size_t>(end - offset));
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      std::wstring wide = utf8_to_wide(line);
      if (wide.empty() && !line.empty()) {
        const int length = MultiByteToWideChar(CP_ACP, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        wide.resize(length); MultiByteToWideChar(CP_ACP, 0, line.data(), static_cast<int>(line.size()), wide.data(), length);
      }
      const bool selected = state->has_selection && state->selection_offset < end &&
                            state->selection_offset + state->selection_length >= offset;
      if (selected) {
        RECT row{0, y, client.right, y + height};
        SetDCBrushColor(dc, state->dark ? RGB(62, 95, 135) : GetSysColor(COLOR_HIGHLIGHT));
        FillRect(dc, &row, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetTextColor(dc, state->dark ? RGB(255, 255, 255) : GetSysColor(COLOR_HIGHLIGHTTEXT));
      }
      TextOutW(dc, 6, y, wide.data(), static_cast<int>(wide.size()));
      if (selected) SetTextColor(dc, foreground);
      offset = end < state->file.size() ? end + 1 : end;
    }
    if (old_font) SelectObject(dc, old_font);
    EndPaint(window, &paint); return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace listopad::app
