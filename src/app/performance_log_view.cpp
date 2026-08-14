#include "performance_log_view.h"

#include "performance_log_controller.h"
#include "listopad/file_io.h"
#include "listopad/strings.h"

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

constexpr wchar_t kClassName[] = L"ListopadPPPerformanceLog";
constexpr UINT kLoadComplete = WM_APP + 90;
constexpr UINT kSetDark = WM_APP + 91;
constexpr int kSummaryId = 1;
constexpr int kFilterId = 2;
constexpr int kCountersId = 3;
constexpr int kSamplesToggleId = 4;
constexpr int kSamplesId = 5;
constexpr int kFilterCount = 6;
constexpr int kCounterColumns = 8;

struct CreateOptions {
  bool russian{true};
  PerformanceLogUiState ui;
};

struct State {
  PerformanceLogController controller;
  PerformanceLogDocument document;
  std::vector<std::size_t> visible_counters;
  HWND summary{nullptr};
  HWND filter{nullptr};
  HWND counters{nullptr};
  HWND samples_toggle{nullptr};
  HWND samples{nullptr};
  HFONT font{nullptr};
  HBRUSH background_brush{nullptr};
  bool russian{true};
  bool dark{false};
  bool loaded{false};
  std::wstring status;
  PerformanceLogUiState ui;

  ~State() {
    if (background_brush) DeleteObject(background_brush);
  }
};

struct Palette {
  COLORREF background;
  COLORREF foreground;
  COLORREF muted;
  COLORREF grid;
  COLORREF line;
};

State* state_for(const HWND window) noexcept {
  return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

COLORREF interpretation_color(const PerformanceLogInterpretationKind kind,
                              const bool dark) noexcept {
  switch (kind) {
    case PerformanceLogInterpretationKind::Warning:
      return dark ? RGB(230, 171, 60) : RGB(176, 112, 0);
    case PerformanceLogInterpretationKind::Error:
      return dark ? RGB(236, 118, 96) : RGB(191, 51, 20);
    case PerformanceLogInterpretationKind::Information:
      break;
  }
  return dark ? RGB(96, 168, 232) : RGB(0, 90, 181);
}

Palette palette_for(const State& state) noexcept {
  if (state.dark) {
    return {RGB(30, 30, 30), RGB(225, 225, 225), RGB(150, 150, 150),
            RGB(64, 64, 64), RGB(96, 168, 232)};
  }
  return {GetSysColor(COLOR_WINDOW), GetSysColor(COLOR_WINDOWTEXT),
          GetSysColor(COLOR_GRAYTEXT), RGB(214, 214, 214), RGB(0, 90, 181)};
}

const PerformanceCounterSeries* selected_series(
    const State& state, int* visible_index = nullptr) noexcept {
  if (!state.loaded || state.visible_counters.empty()) return nullptr;
  int selected = state.counters
                     ? ListView_GetNextItem(state.counters, -1, LVNI_SELECTED)
                     : -1;
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >= state.visible_counters.size()) {
    const auto persisted =
        std::find(state.visible_counters.begin(),
                  state.visible_counters.end(), state.ui.selected_counter);
    if (persisted == state.visible_counters.end()) return nullptr;
    selected = static_cast<int>(
        std::distance(state.visible_counters.begin(), persisted));
  }
  if (visible_index) *visible_index = selected;
  const std::size_t index =
      state.visible_counters[static_cast<std::size_t>(selected)];
  if (index >= state.document.counters.size()) return nullptr;
  return &state.document.counters[index];
}

std::wstring format_interval(const std::uint64_t interval_ms,
                             const bool russian) {
  std::array<wchar_t, 32> buffer{};
  if (interval_ms == 0) return russian ? L"—" : L"—";
  if (interval_ms % 1000 == 0) {
    swprintf_s(buffer.data(), buffer.size(), L"%llu %s",
               static_cast<unsigned long long>(interval_ms / 1000),
               russian ? L"с" : L"s");
  } else if (interval_ms >= 1000) {
    swprintf_s(buffer.data(), buffer.size(), L"%.1f %s",
               static_cast<double>(interval_ms) / 1000.0,
               russian ? L"с" : L"s");
  } else {
    swprintf_s(buffer.data(), buffer.size(), L"%llu %s",
               static_cast<unsigned long long>(interval_ms),
               russian ? L"мс" : L"ms");
  }
  std::wstring result = buffer.data();
  if (russian) {
    const std::size_t dot = result.find(L'.');
    if (dot != std::wstring::npos) result[dot] = L',';
  }
  return result;
}

void update_summary(State& state) {
  if (!state.summary) return;
  if (!state.loaded) {
    SetWindowTextW(state.summary, state.status.c_str());
    return;
  }
  std::size_t with_values = 0;
  for (const PerformanceCounterSeries& series : state.document.counters) {
    if (series.has_values()) ++with_values;
  }
  std::wstring summary =
      state.russian ? L"Системный монитор" : L"Performance log";
  if (!state.document.machine.empty()) {
    summary += L" · ";
    summary += utf8_to_wide(state.document.machine);
  }
  summary += L" · ";
  summary += state.russian ? L"счётчиков: " : L"counters: ";
  summary += std::to_wstring(state.document.counters.size());
  // Counters without a single sample are worth calling out; when every
  // counter reported, the extra clause only crowds the bar.
  if (with_values != state.document.counters.size()) {
    summary += state.russian ? L" (с данными: " : L" (with samples: ";
    summary += std::to_wstring(with_values);
    summary += L")";
  }
  summary += L" · ";
  summary += state.russian ? L"отсчётов: " : L"samples: ";
  summary += std::to_wstring(state.document.samples.size());
  summary += L" · ";
  summary += format_interval(
      performance_log_interval_ms(state.document), state.russian);
  summary += L" · ";
  summary += utf8_to_wide(
      performance_log_format_range(state.document, state.russian));
  if (!state.document.time_zone.empty()) {
    summary += L" · ";
    summary += utf8_to_wide(state.document.time_zone);
  } else if (state.document.node_local_time) {
    summary += state.russian ? L" · местное время узла"
                             : L" · node local time";
  }
  if (state.document.truncated) {
    summary += state.russian ? L" · показана часть журнала"
                             : L" · log shown in part";
  }
  SetWindowTextW(state.summary, summary.c_str());
}

void update_samples(State& state) {
  if (!state.samples) return;
  const PerformanceCounterSeries* series = selected_series(state);
  const std::size_t count = series ? series->values.size() : 0;
  ListView_SetItemCountEx(
      state.samples,
      static_cast<int>((std::min)(
          count,
          static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
      LVSICF_NOINVALIDATEALL);
  InvalidateRect(state.samples, nullptr, TRUE);
}

void rebuild_visible(const HWND window, State& state) {
  state.visible_counters.clear();
  if (state.loaded) {
    state.visible_counters.reserve(state.document.counters.size());
    for (std::size_t index = 0; index < state.document.counters.size();
         ++index) {
      if (performance_counter_matches_filter(state.document.counters[index],
                                             state.ui.filter)) {
        state.visible_counters.push_back(index);
      }
    }
  }
  ListView_SetItemCountEx(
      state.counters,
      static_cast<int>((std::min)(
          state.visible_counters.size(),
          static_cast<std::size_t>((std::numeric_limits<int>::max)()))),
      LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  if (!state.visible_counters.empty()) {
    const auto selected =
        std::find(state.visible_counters.begin(),
                  state.visible_counters.end(), state.ui.selected_counter);
    const int selected_row =
        selected == state.visible_counters.end()
            ? 0
            : static_cast<int>(
                  std::distance(state.visible_counters.begin(), selected));
    state.ui.selected_counter =
        state.visible_counters[static_cast<std::size_t>(selected_row)];
    ListView_SetItemState(state.counters, selected_row,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(state.counters, selected_row);
    ListView_EnsureVisible(state.counters, selected_row, FALSE);
  }
  InvalidateRect(state.counters, nullptr, TRUE);
  update_samples(state);
  InvalidateRect(window, nullptr, TRUE);
}

PerformanceLogLayout layout_for(const HWND window, const State& state) {
  RECT client{};
  GetClientRect(window, &client);
  return calculate_performance_log_layout(
      client.right - client.left, client.bottom - client.top,
      static_cast<int>(GetDpiForWindow(window)), state.ui.show_samples);
}

void apply_layout(const HWND window, State& state) {
  const PerformanceLogLayout layout = layout_for(window, state);
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const auto move = [](const HWND control, const LayoutRect& rect) {
    MoveWindow(control, rect.x, rect.y, rect.width, rect.height, TRUE);
  };
  move(state.summary, layout.summary);
  move(state.filter, layout.filter);
  move(state.counters, layout.counters);
  move(state.samples_toggle, layout.samples_toggle);
  move(state.samples, layout.samples);
  ShowWindow(state.samples, state.ui.show_samples ? SW_SHOW : SW_HIDE);

  const std::array<int, kCounterColumns> widths{
      MulDiv(150, dpi, 96), MulDiv(110, dpi, 96), MulDiv(220, dpi, 96),
      MulDiv(88, dpi, 96),  MulDiv(88, dpi, 96),  MulDiv(88, dpi, 96),
      MulDiv(104, dpi, 96), MulDiv(240, dpi, 96)};
  int used = 0;
  for (int index = 0; index + 1 < kCounterColumns; ++index) {
    ListView_SetColumnWidth(state.counters, index, widths[index]);
    used += widths[index];
  }
  const int remaining = layout.counters.width - used -
                        GetSystemMetrics(SM_CXVSCROLL) - MulDiv(6, dpi, 96);
  ListView_SetColumnWidth(state.counters, kCounterColumns - 1,
                          (std::max)(widths[kCounterColumns - 1], remaining));

  const int time_width = MulDiv(190, dpi, 96);
  ListView_SetColumnWidth(state.samples, 0, time_width);
  ListView_SetColumnWidth(
      state.samples, 1,
      (std::max)(MulDiv(120, dpi, 96),
                 layout.samples.width - time_width -
                     GetSystemMetrics(SM_CXVSCROLL) - MulDiv(6, dpi, 96)));
}

void apply_theme(const HWND window, State& state) {
  if (state.background_brush) {
    DeleteObject(state.background_brush);
    state.background_brush = nullptr;
  }
  const Palette palette = palette_for(state);
  state.background_brush = CreateSolidBrush(palette.background);
  for (const HWND list : {state.counters, state.samples}) {
    ListView_SetBkColor(list, palette.background);
    ListView_SetTextBkColor(list, palette.background);
    ListView_SetTextColor(list, palette.foreground);
  }
  for (const HWND control : {state.filter, state.counters,
                             state.samples_toggle, state.samples}) {
    SetWindowTheme(control, state.dark ? L"DarkMode_Explorer" : nullptr,
                   nullptr);
  }
  InvalidateRect(window, nullptr, TRUE);
}

void draw_text_line(const HDC dc, const RECT& bounds, const std::wstring& text,
                    const UINT format) {
  RECT rect = bounds;
  DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
            format | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

std::wstring format_axis_value(const double value, const int decimals,
                               const bool russian) {
  std::array<wchar_t, 48> buffer{};
  swprintf_s(buffer.data(), buffer.size(), L"%.*f", decimals, value);
  std::wstring result = buffer.data();
  if (russian) {
    const std::size_t dot = result.find(L'.');
    if (dot != std::wstring::npos) result[dot] = L',';
  }
  return result;
}

void paint_chart(const HDC dc, const LayoutRect& area, const int dpi,
                 State& state) {
  const Palette palette = palette_for(state);
  RECT frame{area.x, area.y, area.x + area.width, area.y + area.height};
  if (frame.right <= frame.left || frame.bottom <= frame.top) return;

  const HBRUSH background = CreateSolidBrush(palette.background);
  FillRect(dc, &frame, background);
  DeleteObject(background);
  const HPEN border = CreatePen(PS_SOLID, 1, palette.grid);
  const HGDIOBJ previous_pen = SelectObject(dc, border);
  const HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, frame.left, frame.top, frame.right, frame.bottom);
  SelectObject(dc, previous_brush);
  SelectObject(dc, previous_pen);
  DeleteObject(border);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, palette.foreground);
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  const int line_height = metrics.tmHeight + metrics.tmExternalLeading;
  const int padding = (std::max)(4, line_height / 3);

  const PerformanceCounterSeries* series = selected_series(state);
  if (!state.loaded || !series || !series->has_values()) {
    const std::wstring message =
        !state.loaded
            ? state.status
            : (state.russian ? L"Выберите счётчик с отсчётами."
                             : L"Select a counter that has samples.");
    RECT text{frame.left + padding, frame.top + padding,
              frame.right - padding, frame.bottom - padding};
    draw_text_line(dc, text, message, DT_CENTER | DT_VCENTER);
    return;
  }

  const PerformanceLogInterpretation interpretation =
      interpret_performance_counter(*series, state.russian);
  const COLORREF accent =
      interpretation_color(interpretation.kind, state.dark);

  RECT title{frame.left + padding, frame.top + padding, frame.right - padding,
             frame.top + padding + line_height};
  draw_text_line(dc, title, utf8_to_wide(series->path.full), DT_LEFT);
  RECT subtitle{title.left, title.bottom, title.right,
                title.bottom + line_height};
  SetTextColor(dc, accent);
  draw_text_line(dc, subtitle, utf8_to_wide(interpretation.summary), DT_LEFT);
  SetTextColor(dc, palette.muted);

  const int gutter = MulDiv(64, dpi, 96);
  const int axis_height = line_height + padding;
  LayoutRect plot{
      frame.left + padding + gutter,
      subtitle.bottom + padding,
      frame.right - padding - (frame.left + padding + gutter),
      frame.bottom - padding - axis_height - (subtitle.bottom + padding),
  };
  if (plot.width < 8 || plot.height < 8) return;

  const PerformanceChartScale scale =
      performance_chart_scale(series->statistics);
  const HPEN grid_pen = CreatePen(PS_SOLID, 1, palette.grid);
  const HGDIOBJ saved_pen = SelectObject(dc, grid_pen);
  // Axis labels must not collide: a short plot gets the two bounds only.
  const int divisions = plot.height >= line_height * 5 ? 4 : 2;
  for (int division = 0; division <= divisions; ++division) {
    const int y = plot.y + plot.height - 1 -
                  MulDiv(division, plot.height - 1, divisions);
    MoveToEx(dc, plot.x, y, nullptr);
    LineTo(dc, plot.x + plot.width, y);
    const double value =
        scale.minimum + (scale.maximum - scale.minimum) *
                            static_cast<double>(division) / divisions;
    RECT label{frame.left + padding, y - line_height / 2, plot.x - 4,
               y + line_height / 2 + 1};
    draw_text_line(dc, label,
                   format_axis_value(value, scale.decimals, state.russian),
                   DT_RIGHT | DT_VCENTER);
  }
  SelectObject(dc, saved_pen);
  DeleteObject(grid_pen);

  if (!state.document.samples.empty()) {
    RECT left{plot.x, plot.y + plot.height + padding / 2,
              plot.x + plot.width / 2, frame.bottom - padding};
    RECT right{plot.x + plot.width / 2, left.top, plot.x + plot.width,
               left.bottom};
    draw_text_line(dc, left,
                   utf8_to_wide(performance_log_format_timestamp(
                       state.document.samples.front(), false)),
                   DT_LEFT);
    draw_text_line(dc, right,
                   utf8_to_wide(performance_log_format_timestamp(
                       state.document.samples.back(), false)),
                   DT_RIGHT);
  }

  const auto segments =
      performance_chart_segments(plot, series->values, scale);
  const HPEN line_pen = CreatePen(PS_SOLID, (std::max)(1, line_height / 8),
                                  accent);
  const HGDIOBJ saved_line = SelectObject(dc, line_pen);
  for (const auto& segment : segments) {
    if (segment.size() == 1) {
      const PerformanceChartPoint& point = segment.front();
      Rectangle(dc, point.x - 1, point.y - 1, point.x + 2, point.y + 2);
      continue;
    }
    std::vector<POINT> points;
    points.reserve(segment.size());
    for (const PerformanceChartPoint& point : segment) {
      points.push_back({point.x, point.y});
    }
    Polyline(dc, points.data(), static_cast<int>(points.size()));
  }
  SelectObject(dc, saved_line);
  DeleteObject(line_pen);
}

void paint(const HWND window, State& state) {
  PAINTSTRUCT paint_struct{};
  const HDC dc = BeginPaint(window, &paint_struct);
  const int dpi = static_cast<int>(GetDpiForWindow(window));
  const LayoutRect chart = layout_for(window, state).chart;
  if (chart.width > 0 && chart.height > 0) {
    const HDC memory = CreateCompatibleDC(dc);
    const HBITMAP bitmap =
        CreateCompatibleBitmap(dc, chart.width, chart.height);
    if (memory && bitmap) {
      const HGDIOBJ saved_bitmap = SelectObject(memory, bitmap);
      if (state.font) SelectObject(memory, state.font);
      // The chart is drawn in client coordinates and blitted as one block, so
      // resizing or reselecting a counter cannot flash a half-drawn plot.
      SetViewportOrgEx(memory, -chart.x, -chart.y, nullptr);
      paint_chart(memory, chart, dpi, state);
      SetViewportOrgEx(memory, 0, 0, nullptr);
      BitBlt(dc, chart.x, chart.y, chart.width, chart.height, memory, 0, 0,
             SRCCOPY);
      SelectObject(memory, saved_bitmap);
    } else {
      paint_chart(dc, chart, dpi, state);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
  }
  EndPaint(window, &paint_struct);
}

std::wstring counter_cell(const State& state,
                          const PerformanceCounterSeries& series,
                          const int column) {
  const double reference = performance_counter_magnitude(series.statistics);
  const auto aggregate = [&series, reference, &state](const double value) {
    return utf8_to_wide(performance_log_format_scaled_value(
        series.has_values() ? value : kPerformanceLogNoValue, reference,
        state.russian));
  };
  switch (column) {
    case 0:
      return utf8_to_wide(series.path.object);
    case 1:
      return utf8_to_wide(series.path.instance);
    case 2:
      return utf8_to_wide(series.path.counter);
    case 3:
      return aggregate(series.statistics.minimum);
    case 4:
      return aggregate(series.statistics.average);
    case 5:
      return aggregate(series.statistics.maximum);
    case 6:
      return aggregate(series.statistics.last);
    case 7:
      return utf8_to_wide(
          interpret_performance_counter(series, state.russian).summary);
    default:
      break;
  }
  return {};
}

void provide_counter_text(State& state, NMLVDISPINFOW& display) {
  if ((display.item.mask & LVIF_TEXT) == 0 || display.item.iItem < 0 ||
      static_cast<std::size_t>(display.item.iItem) >=
          state.visible_counters.size() ||
      !display.item.pszText || display.item.cchTextMax <= 0) {
    return;
  }
  const std::size_t index =
      state.visible_counters[static_cast<std::size_t>(display.item.iItem)];
  if (index >= state.document.counters.size()) return;
  const std::wstring text = counter_cell(
      state, state.document.counters[index], display.item.iSubItem);
  wcsncpy_s(display.item.pszText,
            static_cast<std::size_t>(display.item.cchTextMax), text.c_str(),
            _TRUNCATE);
}

void provide_sample_text(State& state, NMLVDISPINFOW& display) {
  if ((display.item.mask & LVIF_TEXT) == 0 || display.item.iItem < 0 ||
      !display.item.pszText || display.item.cchTextMax <= 0) {
    return;
  }
  const PerformanceCounterSeries* series = selected_series(state);
  const auto row = static_cast<std::size_t>(display.item.iItem);
  if (!series || row >= series->values.size()) return;
  std::wstring text;
  if (display.item.iSubItem == 0) {
    text = row < state.document.samples.size()
               ? utf8_to_wide(performance_log_format_timestamp(
                     state.document.samples[row], false))
               : L"—";
  } else if (display.item.iSubItem == 1) {
    text = utf8_to_wide(performance_log_format_scaled_value(
        series->values[row],
        performance_counter_magnitude(series->statistics), state.russian));
  }
  wcsncpy_s(display.item.pszText,
            static_cast<std::size_t>(display.item.cchTextMax), text.c_str(),
            _TRUNCATE);
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

// The samples of a counter are the record the projection is derived from, so
// copying them yields exactly the timestamps and values the log contains.
std::wstring samples_as_text(const State& state,
                             const PerformanceCounterSeries& series) {
  std::wstring result = utf8_to_wide(series.path.full);
  result += L"\r\n";
  for (std::size_t index = 0; index < series.values.size(); ++index) {
    result += index < state.document.samples.size()
                  ? utf8_to_wide(performance_log_format_timestamp(
                        state.document.samples[index], true))
                  : L"—";
    result += L'\t';
    result += utf8_to_wide(performance_log_format_scaled_value(
        series.values[index],
        performance_counter_magnitude(series.statistics), state.russian));
    result += L"\r\n";
  }
  return result;
}

}  // namespace

bool PerformanceLogView::register_class(const HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance;
  type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  type.hbrBackground = nullptr;
  return RegisterClassExW(&type) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND PerformanceLogView::create(const HWND parent, const int control_id,
                                const bool russian,
                                const PerformanceLogUiState initial_state) {
  CreateOptions options{.russian = russian, .ui = initial_state};
  return CreateWindowExW(
      0, kClassName, L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_TABSTOP,
      0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)),
      GetModuleHandleW(nullptr), &options);
}

bool PerformanceLogView::open(const HWND window,
                              const std::filesystem::path& path) {
  State* state = state_for(window);
  if (!state) return false;
  state->loaded = false;
  state->document = {};
  state->visible_counters.clear();
  state->status = state->russian ? L"Системный монитор · чтение…"
                                 : L"Performance log · reading…";
  ListView_SetItemCount(state->counters, 0);
  ListView_SetItemCount(state->samples, 0);
  SetWindowTextW(state->summary, state->status.c_str());
  InvalidateRect(window, nullptr, TRUE);
  state->controller.start(window, kLoadComplete, path);
  return true;
}

bool PerformanceLogView::looks_like(const std::filesystem::path& path) {
  if (looks_like_performance_log_name(path)) return true;
  MappedFile file;
  if (!file.open(path) || !file.data()) return false;
  const auto sample_size = static_cast<std::size_t>(
      (std::min)(file.size(), static_cast<std::uint64_t>(4096)));
  return looks_like_performance_log_text(
      {reinterpret_cast<const char*>(file.data()), sample_size});
}

void PerformanceLogView::set_dark(const HWND window, const bool dark) {
  SendMessageW(window, kSetDark, dark ? TRUE : FALSE, 0);
}

bool PerformanceLogView::find_next(const HWND window,
                                   const std::string& pattern,
                                   const SearchOptions options,
                                   const bool wrap) {
  State* state = state_for(window);
  if (!state || !state->loaded || pattern.empty() ||
      state->visible_counters.empty()) {
    return false;
  }
  int selected = -1;
  (void)selected_series(*state, &selected);
  const int count = static_cast<int>(state->visible_counters.size());
  const int first = selected >= 0 ? selected + 1 : 0;
  const int attempts = wrap ? count : count - first;
  for (int offset = 0; offset < attempts; ++offset) {
    const int visible_index = (first + offset) % count;
    const std::size_t counter_index =
        state->visible_counters[static_cast<std::size_t>(visible_index)];
    const PerformanceCounterSeries& series =
        state->document.counters[counter_index];
    std::string subject = series.path.full;
    subject += ' ';
    subject += interpret_performance_counter(series, state->russian).summary;
    const SearchOneResult found =
        search_next(subject, pattern, options, 0, false);
    if (!found.ok || !found.found) continue;
    ListView_SetItemState(state->counters, visible_index,
                          LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state->counters, visible_index, FALSE);
    SetFocus(state->counters);
    return true;
  }
  MessageBeep(MB_ICONINFORMATION);
  return false;
}

bool PerformanceLogView::copy(const HWND window) {
  State* state = state_for(window);
  if (!state) return false;
  const PerformanceCounterSeries* series = selected_series(*state);
  if (!series) return false;
  copy_wide_text(samples_as_text(*state, *series));
  return true;
}

bool PerformanceLogView::select_all(const HWND) {
  // The projection is a read-only single-selection table: there is nothing a
  // select-all could mean here, and Copy already yields the whole series.
  return false;
}

PerformanceLogUiState PerformanceLogView::ui_state(const HWND window) {
  const State* state = state_for(window);
  return state ? state->ui : PerformanceLogUiState{};
}

LRESULT CALLBACK PerformanceLogView::window_proc(const HWND window,
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
    constexpr DWORD list_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                 LVS_REPORT | LVS_OWNERDATA |
                                 LVS_SINGLESEL | LVS_SHOWSELALWAYS;
    state->counters = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", list_style, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCountersId)), nullptr,
        nullptr);
    state->samples_toggle = CreateWindowExW(
        0, L"BUTTON",
        state->russian ? L"Показать отсчёты" : L"Show samples",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0,
        window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSamplesToggleId)),
        nullptr, nullptr);
    state->samples = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", list_style, 0, 0, 0, 0, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSamplesId)), nullptr,
        nullptr);

    const std::array<const wchar_t*, kFilterCount> filters =
        state->russian
            ? std::array<const wchar_t*, kFilterCount>{
                  L"Все счётчики", L"Только с данными", L"С отклонениями",
                  L"Процессор", L"Память", L"Диск"}
            : std::array<const wchar_t*, kFilterCount>{
                  L"All counters", L"With samples", L"Deviations",
                  L"Processor", L"Memory", L"Disk"};
    for (const wchar_t* filter : filters) {
      SendMessageW(state->filter, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(filter));
    }
    SendMessageW(state->filter, CB_SETCURSEL,
                 static_cast<WPARAM>(state->ui.filter), 0);

    for (const HWND list : {state->counters, state->samples}) {
      ListView_SetExtendedListViewStyle(
          list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                    LVS_EX_LABELTIP);
    }
    const std::array<const wchar_t*, kCounterColumns> columns =
        state->russian
            ? std::array<const wchar_t*, kCounterColumns>{
                  L"Объект", L"Экземпляр", L"Счётчик", L"Мин", L"Среднее",
                  L"Макс", L"Последнее", L"Оценка"}
            : std::array<const wchar_t*, kCounterColumns>{
                  L"Object", L"Instance", L"Counter", L"Min", L"Average",
                  L"Max", L"Last", L"Assessment"};
    for (int index = 0; index < kCounterColumns; ++index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
      column.pszText = const_cast<wchar_t*>(columns[index]);
      column.iSubItem = index;
      column.cx = 100;
      ListView_InsertColumn(state->counters, index, &column);
    }
    const std::array<const wchar_t*, 2> sample_columns =
        state->russian
            ? std::array<const wchar_t*, 2>{L"Время", L"Значение"}
            : std::array<const wchar_t*, 2>{L"Time", L"Value"};
    for (int index = 0; index < 2; ++index) {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_SUBITEM | LVCF_WIDTH;
      column.pszText = const_cast<wchar_t*>(sample_columns[index]);
      column.iSubItem = index;
      column.cx = 100;
      ListView_InsertColumn(state->samples, index, &column);
    }
    Button_SetCheck(state->samples_toggle,
                    state->ui.show_samples ? BST_CHECKED : BST_UNCHECKED);
    ShowWindow(state->samples, state->ui.show_samples ? SW_SHOW : SW_HIDE);
    apply_theme(window, *state);
    apply_layout(window, *state);
    return 0;
  } else if (message == WM_SIZE && state) {
    apply_layout(window, *state);
    InvalidateRect(window, nullptr, TRUE);
    return 0;
  } else if (message == WM_SETFONT && state) {
    state->font = reinterpret_cast<HFONT>(wparam);
    for (const HWND control :
         {state->summary, state->filter, state->counters,
          state->samples_toggle, state->samples}) {
      SendMessageW(control, WM_SETFONT, wparam, TRUE);
    }
    return 0;
  } else if (message == WM_SETFOCUS && state) {
    SetFocus(state->counters);
    return 0;
  } else if (message == WM_PAINT && state) {
    paint(window, *state);
    return 0;
  } else if (message == kSetDark && state) {
    state->dark = wparam != FALSE;
    apply_theme(window, *state);
    return 0;
  } else if (message == kLoadComplete && state) {
    std::unique_ptr<PerformanceLogLoadResult> completed(
        reinterpret_cast<PerformanceLogLoadResult*>(lparam));
    if (!completed || !state->controller.accepts(*completed)) return 0;
    if (completed->error != 0) {
      state->status =
          (state->russian ? L"Не удалось прочитать журнал: "
                          : L"Unable to read the performance log: ") +
          (completed->pdh_status
               ? pdh_status_message(completed->error)
               : win32_error_message(completed->error));
      SetWindowTextW(state->summary, state->status.c_str());
      InvalidateRect(window, nullptr, TRUE);
      return 0;
    }
    state->document = std::move(completed->document);
    state->loaded = true;
    if (state->document.counters.empty()) {
      state->loaded = false;
      state->status =
          state->russian
              ? L"Счётчики не найдены. Переключитесь в другое представление."
              : L"No counters found. Switch to another view.";
      SetWindowTextW(state->summary, state->status.c_str());
      InvalidateRect(window, nullptr, TRUE);
      return 0;
    }
    update_summary(*state);
    rebuild_visible(window, *state);
    return 0;
  } else if (message == WM_COMMAND && state) {
    const int id = LOWORD(wparam);
    const int notification = HIWORD(wparam);
    if (id == kFilterId && notification == CBN_SELCHANGE) {
      const LRESULT selected =
          SendMessageW(state->filter, CB_GETCURSEL, 0, 0);
      state->ui.filter =
          selected >= 0 &&
                  selected <= static_cast<LRESULT>(PerformanceLogFilter::Disk)
              ? static_cast<PerformanceLogFilter>(selected)
              : PerformanceLogFilter::All;
      rebuild_visible(window, *state);
      return 0;
    }
    if (id == kSamplesToggleId && notification == BN_CLICKED) {
      state->ui.show_samples =
          Button_GetCheck(state->samples_toggle) == BST_CHECKED;
      apply_layout(window, *state);
      InvalidateRect(window, nullptr, TRUE);
      return 0;
    }
  } else if (message == WM_NOTIFY && state) {
    const auto* header = reinterpret_cast<NMHDR*>(lparam);
    if (header && header->hwndFrom == state->counters) {
      if (header->code == LVN_GETDISPINFOW) {
        provide_counter_text(*state,
                             *reinterpret_cast<NMLVDISPINFOW*>(lparam));
        return 0;
      }
      if (header->code == LVN_ITEMCHANGED) {
        const auto* change = reinterpret_cast<NMLISTVIEW*>(lparam);
        if ((change->uChanged & LVIF_STATE) != 0 &&
            (change->uNewState & LVIS_SELECTED) != 0 &&
            change->iItem >= 0 &&
            static_cast<std::size_t>(change->iItem) <
                state->visible_counters.size()) {
          state->ui.selected_counter =
              state->visible_counters[static_cast<std::size_t>(
                  change->iItem)];
          update_samples(*state);
          const LayoutRect chart = layout_for(window, *state).chart;
          RECT invalid{chart.x, chart.y, chart.x + chart.width,
                       chart.y + chart.height};
          InvalidateRect(window, &invalid, TRUE);
        }
        return 0;
      }
    }
    if (header && header->hwndFrom == state->samples &&
        header->code == LVN_GETDISPINFOW) {
      provide_sample_text(*state, *reinterpret_cast<NMLVDISPINFOW*>(lparam));
      return 0;
    }
  } else if (message == WM_CTLCOLORSTATIC && state) {
    const Palette palette = palette_for(*state);
    const HDC dc = reinterpret_cast<HDC>(wparam);
    SetTextColor(dc, palette.foreground);
    SetBkColor(dc, palette.background);
    return reinterpret_cast<LRESULT>(state->background_brush
                                         ? state->background_brush
                                         : GetSysColorBrush(COLOR_WINDOW));
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
