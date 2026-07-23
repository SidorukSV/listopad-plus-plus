#include "editor_window.h"

#include "document_map.h"
#include "hex_view_window.h"
#include "large_file_view.h"
#include "resource.h"
#include "listopad/encoding.h"
#include "listopad/file_io.h"
#include "listopad/formatter.h"
#include "listopad/language.h"
#include "listopad/lexers.h"
#include "listopad/search.h"
#include "listopad/shell_registration.h"
#include "listopad/strings.h"
#include "listopad/version.h"

#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <Scintilla.h>
#include <SciLexer.h>
#include <ILexer.h>
#include <Lexilla.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

extern "C" int Scintilla_RegisterClasses(void* hInstance);

namespace listopad::app {
namespace {

constexpr wchar_t kWindowClass[] = L"ListopadPPMainWindow";
constexpr UINT kLanguageFirst = 3000;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr int kMinWindowWidth = 480;
constexpr int kMinWindowHeight = 320;

// Turn a persisted window rectangle into one guaranteed to land on a currently
// connected monitor. This is the safeguard against a saved position that has
// since drifted off-screen — e.g. the window was on an external monitor that is
// no longer attached, or the display arrangement/resolution changed. Returns
// false when nothing sensible can be recovered so the caller uses its defaults.
bool resolve_visible_bounds(const listopad::WindowBounds& saved, RECT& out) {
  if (!saved.valid) return false;

  int width = std::max(saved.width, kMinWindowWidth);
  int height = std::max(saved.height, kMinWindowHeight);
  RECT rect{saved.x, saved.y, saved.x + width, saved.y + height};

  // Pick the monitor the saved rectangle overlaps most; NULL means it lies fully
  // outside every monitor, which is exactly the off-screen case we guard against.
  HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
  if (!monitor) return false;

  MONITORINFO info{sizeof(info)};
  if (!GetMonitorInfoW(monitor, &info)) return false;
  const RECT work = info.rcWork;

  // Never let the window exceed the monitor's usable area, then slide it fully
  // inside that area so the title bar always stays grabbable.
  width = std::min<int>(width, work.right - work.left);
  height = std::min<int>(height, work.bottom - work.top);
  int x = std::clamp<int>(saved.x, work.left, work.right - width);
  int y = std::clamp<int>(saved.y, work.top, work.bottom - height);

  out = RECT{x, y, x + width, y + height};
  return true;
}

enum class PreferredAppMode : int {
  Default,
  AllowDark,
  ForceDark,
  ForceLight,
};

struct DarkModeApi {
  using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
  using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
  using FlushMenuThemesFn = void(WINAPI*)();

  SetPreferredAppModeFn set_preferred_app_mode{nullptr};
  AllowDarkModeForWindowFn allow_dark_mode_for_window{nullptr};
  FlushMenuThemesFn flush_menu_themes{nullptr};

  DarkModeApi() {
    const HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    set_preferred_app_mode = load<SetPreferredAppModeFn>(uxtheme, 135);
    allow_dark_mode_for_window = load<AllowDarkModeForWindowFn>(uxtheme, 133);
    flush_menu_themes = load<FlushMenuThemesFn>(uxtheme, 136);
  }

 private:
  template <typename Function>
  static Function load(HMODULE module, WORD ordinal) {
    const FARPROC raw = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
    Function result{};
    static_assert(sizeof(result) == sizeof(raw));
    std::memcpy(&result, &raw, sizeof(result));
    return result;
  }
};

DarkModeApi& dark_mode_api() {
  static DarkModeApi api;
  return api;
}

void set_preferred_app_theme(const std::string_view configured_theme) {
  auto& api = dark_mode_api();
  if (!api.set_preferred_app_mode) return;
  if (configured_theme == "dark") api.set_preferred_app_mode(PreferredAppMode::ForceDark);
  else if (configured_theme == "light") api.set_preferred_app_mode(PreferredAppMode::ForceLight);
  else api.set_preferred_app_mode(PreferredAppMode::AllowDark);
}

LPARAM pointer_param(const void* value) { return reinterpret_cast<LPARAM>(value); }
LRESULT sci(HWND editor, UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
  return SendMessageW(editor, message, wparam, lparam);
}

std::string control_text_utf8(HWND control) {
  const int length = GetWindowTextLengthW(control);
  std::wstring text(static_cast<std::size_t>(length), L'\0');
  GetWindowTextW(control, text.data(), length + 1);
  return wide_to_utf8(text);
}

bool system_dark_theme() {
  DWORD value = 1, size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER,
               L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value == 0;
}

std::wstring eol_name(const EolMode mode) {
  switch (mode) {
    case EolMode::Lf: return L"LF";
    case EolMode::Cr: return L"CR";
    case EolMode::Mixed: return L"Mixed EOL";
    default: return L"CRLF";
  }
}

enum class SearchJobKind { Find, ReplaceAll };

struct SearchTabResult {
  int index{-1};
  std::size_t base{0};
  std::size_t caret{0};
  std::string original;
  std::string subject;
  SearchResult found;
  ReplaceResult replaced;
};

struct SearchJobResult {
  SearchJobKind kind{SearchJobKind::Find};
  std::uint64_t generation{0};
  bool wrap{true};
  std::vector<SearchTabResult> tabs;
};

}  // namespace

EditorWindow::EditorWindow(HINSTANCE instance, Settings settings)
    : instance_(instance), settings_(std::move(settings)),
      watcher_([this](const std::filesystem::path& path) {
        auto* copy = new std::filesystem::path(path);
        if (!window_ || !PostMessageW(window_, kExternalChangeMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
      }) {
  dark_ = settings_.theme == "dark" || (settings_.theme == "system" && system_dark_theme());
}

EditorWindow::~EditorWindow() {
  search_thread_.request_stop();
  watcher_.clear(); elevated_.close();
  if (editor_font_) DeleteObject(editor_font_);
  if (tab_font_) DeleteObject(tab_font_);
  if (icon_font_) DeleteObject(icon_font_);
  if (icon_font_resource_) RemoveFontMemResourceEx(icon_font_resource_);
  if (toolbar_images_) ImageList_Destroy(toolbar_images_);
  if (window_brush_) DeleteObject(window_brush_);
  if (panel_brush_) DeleteObject(panel_brush_);
  if (field_brush_) DeleteObject(field_brush_);
  if (banner_brush_) DeleteObject(banner_brush_);
}

bool EditorWindow::create(const int show_command) {
  set_preferred_app_theme(settings_.theme);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  Scintilla_RegisterClasses(instance_);
  LargeFileView::register_class(instance_);
  HexViewWindow::register_class(instance_);

  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance_; type.lpfnWndProc = window_proc; type.lpszClassName = kWindowClass;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  // LoadIconW only ever yields SM_CXICON, so reusing it as the small icon makes
  // the shell downscale 32x32 to 16x16 instead of picking the dedicated frames
  // the multi-size .ico already carries.
  const auto load_icon = [this](const int metric) {
    return static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_LISTOPAD), IMAGE_ICON,
                                         GetSystemMetrics(metric == SM_CXICON ? SM_CXICON : SM_CXSMICON),
                                         GetSystemMetrics(metric == SM_CXICON ? SM_CYICON : SM_CYSMICON),
                                         LR_DEFAULTCOLOR));
  };
  type.hIcon = load_icon(SM_CXICON);
  type.hIconSm = load_icon(SM_CXSMICON);
  if (!type.hIcon) type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  if (!type.hIconSm) type.hIconSm = type.hIcon;
  type.hbrBackground = nullptr;
  if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, width = 1100, height = 760;
  if (RECT bounds{}; resolve_visible_bounds(settings_.window, bounds)) {
    x = bounds.left; y = bounds.top;
    width = bounds.right - bounds.left; height = bounds.bottom - bounds.top;
  }
  // Honour the maximized state even when the stored normal rectangle was rejected
  // as off-screen: the window then maximizes on the default monitor and its
  // un-maximize size falls back to the created default rather than being lost.
  const bool restore_maximized = settings_.window.valid && settings_.window.maximized;
  window_ = CreateWindowExW(0, kWindowClass, LISTOPAD_PRODUCT_NAME,
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            x, y, width, height,
                            nullptr, nullptr, instance_, this);
  if (!window_) return false;
  // Created at the validated restore rectangle, so maximizing here still leaves
  // that rectangle as the "normal" size to return to when the user un-maximizes.
  ShowWindow(window_, restore_maximized ? SW_SHOWMAXIMIZED : show_command);
  // The first ShowWindow after process start can be overridden by the launcher's
  // STARTUPINFO (Explorer and Start-Process force SW_SHOWNORMAL), which silently
  // drops the restored maximized state. A second call is exempt from that rule.
  if (restore_maximized) ShowWindow(window_, SW_SHOWMAXIMIZED);
  UpdateWindow(window_); return true;
}

void EditorWindow::persist_window_bounds() {
  if (!window_) return;
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (!GetWindowPlacement(window_, &placement)) return;
  const bool maximized =
      placement.showCmd == SW_SHOWMAXIMIZED ||
      (placement.showCmd == SW_SHOWMINIMIZED && (placement.flags & WPF_RESTORETOMAXIMIZED));
  // For a maximized or minimized window rcNormalPosition is the right thing to
  // keep — the size to return to on un-maximize — while the live frame would be
  // the full-screen or an off-screen rectangle. For an ordinary window it matches
  // the frame; for an Aero-snapped one (half the screen), rcNormalPosition still
  // holds the pre-snap size, so only GetWindowRect captures where the window
  // actually sits and lets it reopen snapped to the same edge.
  RECT rect = placement.rcNormalPosition;
  if (placement.showCmd != SW_SHOWMAXIMIZED && placement.showCmd != SW_SHOWMINIMIZED) {
    if (RECT frame{}; GetWindowRect(window_, &frame)) rect = frame;
  }
  settings_.window.valid = true;
  settings_.window.maximized = maximized;
  settings_.window.x = rect.left;
  settings_.window.y = rect.top;
  settings_.window.width = rect.right - rect.left;
  settings_.window.height = rect.bottom - rect.top;
  save_settings(settings_);
}

LRESULT CALLBACK EditorWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  EditorWindow* self = reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<EditorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->dispatch(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT EditorWindow::dispatch(const UINT message, const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
    case WM_CREATE: return on_create() ? 0 : -1;
    case WM_SIZE: on_size(); return 0;
    case WM_COMMAND: on_command(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam)); return 0;
    case WM_NOTIFY: {
      auto& header = *reinterpret_cast<NMHDR*>(lparam);
      if (header.hwndFrom == toolbar_ && header.code == NM_CUSTOMDRAW)
        return draw_toolbar(*reinterpret_cast<NMTBCUSTOMDRAW*>(lparam));
      if (header.code == TTN_GETDISPINFOW) {
        toolbar_tooltip(*reinterpret_cast<NMTTDISPINFOW*>(lparam));
        return 0;
      }
      on_notify(header);
      return 0;
    }
    case WM_NCACTIVATE:
    case WM_NCPAINT: {
      const LRESULT result = DefWindowProcW(window_, message, wparam, lparam);
      // The default frame leaves a light 1px seam under the menu bar in dark
      // mode; repaint it with the menu background so the bar blends into it.
      paint_menu_underline();
      return result;
    }
    case WM_MEASUREITEM: {
      auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
      if (!item || item->CtlType != ODT_MENU || item->itemData == 0)
        return DefWindowProcW(window_, message, wparam, lparam);
      const auto* visual = reinterpret_cast<const MenuVisual*>(item->itemData);
      NONCLIENTMETRICSW metrics{sizeof(metrics)};
      SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
      HFONT font = CreateFontIndirectW(&metrics.lfMenuFont);
      HDC dc = GetDC(window_);
      HGDIOBJ previous = font ? SelectObject(dc, font) : nullptr;
      RECT extent{};
      DrawTextW(dc, visual->text.c_str(), -1, &extent,
                DT_CALCRECT | DT_SINGLELINE);
      if (previous) SelectObject(dc, previous);
      ReleaseDC(window_, dc);
      if (font) DeleteObject(font);
      item->itemWidth = static_cast<UINT>(extent.right - extent.left + 20);
      item->itemHeight = static_cast<UINT>((std::max)(
          GetSystemMetrics(SM_CYMENU), static_cast<int>(extent.bottom - extent.top) + 8));
      return TRUE;
    }
    case WM_DRAWITEM: {
      auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (item && item->CtlType == ODT_STATIC && item->hwndItem == banner_) {
        FillRect(item->hDC, &item->rcItem,
                 banner_brush_ ? banner_brush_ : GetSysColorBrush(COLOR_INFOBK));
        std::array<wchar_t, 512> text{};
        GetWindowTextW(banner_, text.data(), static_cast<int>(text.size()));
        RECT text_rect = item->rcItem;
        const UINT dpi = GetDpiForWindow(banner_);
        text_rect.left += MulDiv(12, dpi ? static_cast<int>(dpi) : 96, 96);
        text_rect.right = (std::max)(
            text_rect.left,
            text_rect.right - MulDiv(278, dpi ? static_cast<int>(dpi) : 96, 96));
        const HFONT font = reinterpret_cast<HFONT>(SendMessageW(banner_, WM_GETFONT, 0, 0));
        const HGDIOBJ previous_font = SelectObject(
            item->hDC, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
        const int previous_mode = SetBkMode(item->hDC, TRANSPARENT);
        const COLORREF previous_text = SetTextColor(
            item->hDC, dark_ ? RGB(238, 238, 238) : GetSysColor(COLOR_INFOTEXT));
        DrawTextW(item->hDC, text.data(), -1, &text_rect,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetTextColor(item->hDC, previous_text);
        SetBkMode(item->hDC, previous_mode);
        SelectObject(item->hDC, previous_font);
        return TRUE;
      }
      if (!item || item->CtlType != ODT_MENU || item->itemData == 0)
        return DefWindowProcW(window_, message, wparam, lparam);
      const auto* visual = reinterpret_cast<const MenuVisual*>(item->itemData);
      const bool selected = (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
      const bool disabled = (item->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
      const COLORREF background = dark_
          ? (selected ? RGB(63, 63, 70) : RGB(37, 37, 38))
          : (selected ? GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_MENU));
      const COLORREF foreground = dark_
          ? (disabled ? RGB(110, 110, 110) : RGB(238, 238, 238))
          : (disabled ? GetSysColor(COLOR_GRAYTEXT)
                      : selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_MENUTEXT));
      SetDCBrushColor(item->hDC, background);
      FillRect(item->hDC, &item->rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      const int previous_mode = SetBkMode(item->hDC, TRANSPARENT);
      const COLORREF previous_text = SetTextColor(item->hDC, foreground);
      RECT text_rect = item->rcItem;
      text_rect.left += 10;
      text_rect.right -= 10;
      UINT draw_flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
      BOOL keyboard_cues = FALSE;
      SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &keyboard_cues, 0);
      const bool menu_active = (item->itemState & (ODS_FOCUS | ODS_SELECTED)) != 0;
      const bool hide_accelerators = !keyboard_cues && !menu_active;
      if (hide_accelerators) draw_flags |= DT_HIDEPREFIX;
      DrawTextW(item->hDC, visual->text.c_str(), -1, &text_rect,
                draw_flags);
      SetTextColor(item->hDC, previous_text);
      SetBkMode(item->hDC, previous_mode);
      return TRUE;
    }
    case kOpenRequestMessage: {
      std::unique_ptr<ipc::OpenFilesRequest> request(reinterpret_cast<ipc::OpenFilesRequest*>(lparam));
      if (request) open_request(*request); return 0;
    }
    case kExternalChangeMessage: {
      std::unique_ptr<std::filesystem::path> path(reinterpret_cast<std::filesystem::path*>(lparam));
      if (path) handle_external_change(*path); return 0;
    }
    case kSearchResultMessage:
      handle_search_result(reinterpret_cast<void*>(lparam));
      return 0;
    case WM_SETTINGCHANGE:
      if (settings_.theme == "system") {
        const bool changed = dark_ != system_dark_theme();
        dark_ = system_dark_theme();
        if (changed) apply_window_theme();
      }
      return 0;
    case WM_SYSCOLORCHANGE:
      if (!dark_) { recreate_theme_brushes(); RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); }
      return 0;
    case WM_ERASEBKGND: {
      RECT client{}; GetClientRect(window_, &client);
      FillRect(reinterpret_cast<HDC>(wparam), &client, window_brush_ ? window_brush_ : GetSysColorBrush(COLOR_WINDOW));
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      const HWND control = reinterpret_cast<HWND>(lparam);
      const bool banner = control == banner_;
      const COLORREF background = banner
          ? (dark_ ? RGB(79, 69, 25) : RGB(255, 244, 196))
          : (dark_ ? RGB(37, 37, 38) : GetSysColor(COLOR_BTNFACE));
      SetTextColor(reinterpret_cast<HDC>(wparam), dark_ ? RGB(238, 238, 238) : GetSysColor(COLOR_BTNTEXT));
      SetBkColor(reinterpret_cast<HDC>(wparam), background);
      return reinterpret_cast<LRESULT>(banner ? banner_brush_ : panel_brush_);
    }
    case WM_CTLCOLOREDIT:
      SetTextColor(reinterpret_cast<HDC>(wparam), dark_ ? RGB(238, 238, 238) : GetSysColor(COLOR_WINDOWTEXT));
      SetBkColor(reinterpret_cast<HDC>(wparam), dark_ ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW));
      return reinterpret_cast<LRESULT>(field_brush_);
    case WM_THEMECHANGED:
      RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
      return 0;
    case WM_CLOSE:
      for (int index = static_cast<int>(documents_.size()) - 1; index >= 0; --index) if (!confirm_close(documents_[index])) return 0;
      persist_window_bounds();
      DestroyWindow(window_); return 0;
    case WM_ENDSESSION:
      // Windows is shutting down or the user is logging off, so WM_CLOSE may
      // never arrive — capture the geometry here too. wparam is TRUE only when
      // the session is actually ending (a cancelled shutdown sends FALSE).
      if (wparam) persist_window_bounds();
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(window_, message, wparam, lparam);
  }
}

bool EditorWindow::on_create() {
  rebuild_menu();
  tabs_ = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                          0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_TAB), instance_, nullptr);
  status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE,
                            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_STATUS), instance_, nullptr);
  SetWindowSubclass(tabs_, tabs_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
  SetWindowSubclass(status_, status_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
  banner_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPSIBLINGS | SS_OWNERDRAW,
                            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_BANNER), instance_, nullptr);
  reload_button_ = CreateWindowExW(0, L"BUTTON", tr(L"Перезагрузить", L"Reload"),
                                    WS_CHILD | WS_CLIPSIBLINGS | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_BANNER_RELOAD), instance_, nullptr);
  keep_button_ = CreateWindowExW(0, L"BUTTON", tr(L"Оставить", L"Keep"),
                                  WS_CHILD | WS_CLIPSIBLINGS | BS_PUSHBUTTON,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_BANNER_KEEP), instance_, nullptr);
  search_panel_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_SEARCH_PANEL), instance_, nullptr);
  find_text_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                               0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_FIND_TEXT), instance_, nullptr);
  replace_text_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                  0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_REPLACE_TEXT), instance_, nullptr);
  find_button_ = CreateWindowExW(0, L"BUTTON", tr(L"Найти далее", L"Find next"), WS_CHILD,
                                 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_FIND_NEXT), instance_, nullptr);
  replace_button_ = CreateWindowExW(0, L"BUTTON", tr(L"Заменить", L"Replace"), WS_CHILD,
                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_REPLACE_ONE), instance_, nullptr);
  replace_all_button_ = CreateWindowExW(0, L"BUTTON", tr(L"Заменить всё", L"Replace all"), WS_CHILD,
                                        0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_REPLACE_ALL), instance_, nullptr);
  regex_check_ = CreateWindowExW(0, L"BUTTON", L".*", WS_CHILD | BS_AUTOCHECKBOX,
                                 0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_REGEX), instance_, nullptr);
  case_check_ = CreateWindowExW(0, L"BUTTON", L"Aa", WS_CHILD | BS_AUTOCHECKBOX,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_MATCH_CASE), instance_, nullptr);
  all_tabs_check_ = CreateWindowExW(0, L"BUTTON", tr(L"Все вкладки", L"All tabs"), WS_CHILD | BS_AUTOCHECKBOX,
                                    0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_ALL_TABS), instance_, nullptr);
  whole_word_check_ = CreateWindowExW(0, L"BUTTON", tr(L"Слово", L"Whole word"), WS_CHILD | BS_AUTOCHECKBOX,
                                      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_WHOLE_WORD), instance_, nullptr);
  wrap_check_ = CreateWindowExW(0, L"BUTTON", tr(L"По кругу", L"Wrap"), WS_CHILD | BS_AUTOCHECKBOX,
                                0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_WRAP), instance_, nullptr);
  selection_only_check_ = CreateWindowExW(0, L"BUTTON", tr(L"В выделении", L"Selection"), WS_CHILD | BS_AUTOCHECKBOX,
                                          0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_SELECTION_ONLY), instance_, nullptr);
  Button_SetCheck(wrap_check_, BST_CHECKED);

  HDC window_dc = GetDC(window_);
  const int logical_dpi = window_dc ? GetDeviceCaps(window_dc, LOGPIXELSY) : 96;
  if (window_dc) ReleaseDC(window_, window_dc);
  dpi_ = logical_dpi;
  create_toolbar();
  // Tabs shrink to their label (capped in fit_tab_title) instead of filling a
  // fixed minimum, with a little breathing room top and bottom.
  SendMessageW(tabs_, TCM_SETPADDING, 0,
               MAKELPARAM(MulDiv(20, logical_dpi, 96), MulDiv(7, logical_dpi, 96)));
  SendMessageW(tabs_, TCM_SETMINTABWIDTH, 0, MulDiv(70, logical_dpi, 96));
  // A proportional UI font for the tab strip reads better than the default
  // fixed shell font; the tab control uses it to size and lay out each tab.
  tab_font_ = CreateFontW(-MulDiv(9, logical_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                          FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
  SendMessageW(tabs_, WM_SETFONT, reinterpret_cast<WPARAM>(tab_font_), TRUE);
  editor_font_ = CreateFontW(-MulDiv(settings_.font_size, logical_dpi, 72),
                             0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, utf8_to_wide(settings_.font_face).c_str());
  apply_window_theme();
  add_empty_tab(); update_layout(); return true;
}

void EditorWindow::load_icon_font() {
  // Load the bundled Phosphor subset into the process's private font table so
  // the toolbar can render its glyphs by name; recolouring is just SetTextColor.
  const HRSRC resource = FindResourceW(instance_, MAKEINTRESOURCEW(IDR_PHOSPHOR_FONT), RT_RCDATA);
  if (!resource) return;
  const HGLOBAL loaded = LoadResource(instance_, resource);
  void* data = loaded ? LockResource(loaded) : nullptr;
  const DWORD size = SizeofResource(instance_, resource);
  if (!data || size == 0) return;
  DWORD fonts = 0;
  icon_font_resource_ = AddFontMemResourceEx(data, size, nullptr, &fonts);
  if (!icon_font_resource_) return;
  icon_font_ = CreateFontW(-MulDiv(17, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"ListopadPhosphor");
}

void EditorWindow::create_toolbar() {
  load_icon_font();
  toolbar_ = CreateWindowExW(
      0, TOOLBARCLASSNAMEW, L"",
      WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS |
          CCS_NODIVIDER | CCS_NORESIZE | CCS_NOPARENTALIGN,
      0, 0, 0, 0, window_, reinterpret_cast<HMENU>(IDC_TOOLBAR), instance_, nullptr);
  if (!toolbar_) return;
  SendMessageW(toolbar_, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
  // A blank image list of the desired glyph size reserves each button's width;
  // the glyphs themselves are painted from scratch in draw_toolbar().
  const int icon = MulDiv(16, dpi_, 96);
  toolbar_images_ = ImageList_Create(icon, icon, ILC_COLOR32, 1, 0);
  ImageList_SetImageCount(toolbar_images_, 1);
  SendMessageW(toolbar_, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(toolbar_images_));
  SendMessageW(toolbar_, TB_SETPADDING, 0,
               MAKELPARAM(MulDiv(11, dpi_, 96), MulDiv(9, dpi_, 96)));
  const auto button = [](int command) {
    TBBUTTON entry{};
    entry.iBitmap = 0;
    entry.idCommand = command;
    entry.fsState = TBSTATE_ENABLED;
    entry.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE;
    return entry;
  };
  const auto separator = []() {
    TBBUTTON entry{};
    entry.fsStyle = BTNS_SEP;
    return entry;
  };
  std::array<TBBUTTON, 13> buttons{
      button(IDM_FILE_NEW),   button(IDM_FILE_OPEN),  button(IDM_FILE_SAVE),
      separator(),            button(IDM_EDIT_UNDO),  button(IDM_EDIT_REDO),
      separator(),            button(IDM_EDIT_CUT),   button(IDM_EDIT_COPY),
      button(IDM_EDIT_PASTE), separator(),            button(IDM_SEARCH_FIND),
      button(IDM_SEARCH_REPLACE)};
  SendMessageW(toolbar_, TB_ADDBUTTONSW, buttons.size(),
               reinterpret_cast<LPARAM>(buttons.data()));
  SendMessageW(toolbar_, TB_AUTOSIZE, 0, 0);
}

int EditorWindow::toolbar_height() const {
  if (!toolbar_ || !IsWindowVisible(toolbar_)) return 0;
  SIZE size{};
  if (SendMessageW(toolbar_, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&size)) &&
      size.cy > 0)
    return size.cy + MulDiv(6, dpi_, 96);
  return MulDiv(34, dpi_, 96);
}

LRESULT EditorWindow::draw_toolbar(NMTBCUSTOMDRAW& custom) {
  NMCUSTOMDRAW& base = custom.nmcd;
  const COLORREF bar = dark_ ? RGB(45, 45, 48) : GetSysColor(COLOR_BTNFACE);
  switch (base.dwDrawStage) {
    case CDDS_PREPAINT:
      SetDCBrushColor(base.hdc, bar);
      FillRect(base.hdc, &base.rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: {
      const int command = static_cast<int>(base.dwItemSpec);
      RECT rc = base.rc;
      if (command == 0) {  // separator
        const int x = (rc.left + rc.right) / 2;
        RECT line{x, rc.top + MulDiv(5, dpi_, 96), x + (std::max)(1, MulDiv(1, dpi_, 96)),
                  rc.bottom - MulDiv(5, dpi_, 96)};
        SetDCBrushColor(base.hdc, dark_ ? RGB(70, 70, 74) : RGB(200, 200, 200));
        FillRect(base.hdc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return CDRF_SKIPDEFAULT;
      }
      const bool disabled = (base.uItemState & CDIS_DISABLED) != 0;
      const bool pressed = (base.uItemState & (CDIS_SELECTED | CDIS_CHECKED)) != 0;
      const bool hot = (base.uItemState & CDIS_HOT) != 0;
      SetDCBrushColor(base.hdc, bar);
      FillRect(base.hdc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      if (!disabled && (hot || pressed)) {
        SetDCBrushColor(base.hdc,
                        dark_ ? (pressed ? RGB(62, 62, 66) : RGB(55, 55, 58))
                              : (pressed ? RGB(208, 208, 208) : RGB(226, 226, 226)));
        RECT highlight = rc;
        InflateRect(&highlight, -MulDiv(2, dpi_, 96), -MulDiv(2, dpi_, 96));
        FillRect(base.hdc, &highlight, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      }
      draw_toolbar_glyph(base.hdc, command, rc, disabled);
      return CDRF_SKIPDEFAULT;
    }
    default:
      return CDRF_DODEFAULT;
  }
}

void EditorWindow::draw_toolbar_glyph(HDC dc, int command, RECT button, bool disabled) {
  if (!icon_font_) return;
  // Phosphor glyph codepoints (Private Use Area) for each toolbar action.
  wchar_t glyph = 0;
  switch (command) {
    case IDM_FILE_NEW: glyph = 0xE230; break;        // file
    case IDM_FILE_OPEN: glyph = 0xE256; break;       // folder-open
    case IDM_FILE_SAVE: glyph = 0xE248; break;       // floppy-disk
    case IDM_EDIT_UNDO: glyph = 0xE038; break;       // arrow-counter-clockwise
    case IDM_EDIT_REDO: glyph = 0xE036; break;       // arrow-clockwise
    case IDM_EDIT_CUT: glyph = 0xEAE0; break;         // scissors
    case IDM_EDIT_COPY: glyph = 0xE1CA; break;        // copy
    case IDM_EDIT_PASTE: glyph = 0xE196; break;       // clipboard
    case IDM_SEARCH_FIND: glyph = 0xE30C; break;      // magnifying-glass
    case IDM_SEARCH_REPLACE: glyph = 0xE83C; break;   // swap
    default: return;
  }
  const COLORREF ink = disabled ? (dark_ ? RGB(105, 105, 108) : RGB(170, 170, 170))
                                 : (dark_ ? RGB(226, 226, 228) : RGB(70, 70, 74));
  const HGDIOBJ previous_font = SelectObject(dc, icon_font_);
  const int previous_mode = SetBkMode(dc, TRANSPARENT);
  const COLORREF previous_text = SetTextColor(dc, ink);
  DrawTextW(dc, &glyph, 1, &button,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
  SetTextColor(dc, previous_text);
  SetBkMode(dc, previous_mode);
  SelectObject(dc, previous_font);
}

void EditorWindow::toolbar_tooltip(NMTTDISPINFOW& info) const {
  const wchar_t* text = nullptr;
  switch (static_cast<int>(info.hdr.idFrom)) {
    case IDM_FILE_NEW: text = tr(L"Новый (Ctrl+N)", L"New (Ctrl+N)"); break;
    case IDM_FILE_OPEN: text = tr(L"Открыть (Ctrl+O)", L"Open (Ctrl+O)"); break;
    case IDM_FILE_SAVE: text = tr(L"Сохранить (Ctrl+S)", L"Save (Ctrl+S)"); break;
    case IDM_EDIT_UNDO: text = tr(L"Отменить (Ctrl+Z)", L"Undo (Ctrl+Z)"); break;
    case IDM_EDIT_REDO: text = tr(L"Повторить (Ctrl+Y)", L"Redo (Ctrl+Y)"); break;
    case IDM_EDIT_CUT: text = tr(L"Вырезать (Ctrl+X)", L"Cut (Ctrl+X)"); break;
    case IDM_EDIT_COPY: text = tr(L"Копировать (Ctrl+C)", L"Copy (Ctrl+C)"); break;
    case IDM_EDIT_PASTE: text = tr(L"Вставить (Ctrl+V)", L"Paste (Ctrl+V)"); break;
    case IDM_SEARCH_FIND: text = tr(L"Найти (Ctrl+F)", L"Find (Ctrl+F)"); break;
    case IDM_SEARCH_REPLACE: text = tr(L"Заменить (Ctrl+H)", L"Replace (Ctrl+H)"); break;
    default: break;
  }
  if (text) {
    wcsncpy_s(info.szText, text, _TRUNCATE);
    info.lpszText = info.szText;
  }
}

void EditorWindow::paint_menu_underline() {
  if (!dark_ || !GetMenu(window_)) return;
  MENUBARINFO bar{sizeof(bar)};
  if (!GetMenuBarInfo(window_, OBJID_MENU, 0, &bar)) return;
  RECT frame{};
  GetWindowRect(window_, &frame);
  RECT line = bar.rcBar;
  OffsetRect(&line, -frame.left, -frame.top);
  line.top = line.bottom - MulDiv(1, dpi_, 96);
  line.bottom += MulDiv(2, dpi_, 96);
  HDC dc = GetWindowDC(window_);
  if (!dc) return;
  SetDCBrushColor(dc, RGB(37, 37, 38));
  FillRect(dc, &line, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  ReleaseDC(window_, dc);
}

std::wstring EditorWindow::fit_tab_title(std::wstring title) const {
  if (!tabs_ || !tab_font_ || title.empty()) return title;
  HDC dc = GetDC(tabs_);
  if (!dc) return title;
  const HGDIOBJ previous = SelectObject(dc, tab_font_);
  const int max_px = MulDiv(220, dpi_, 96);  // 260 cap minus the tab's horizontal padding
  SIZE size{};
  GetTextExtentPoint32W(dc, title.c_str(), static_cast<int>(title.size()), &size);
  if (size.cx > max_px && title.size() > 1) {
    const std::wstring ellipsis = L"…";
    int low = 0;
    int high = static_cast<int>(title.size());
    while (low < high) {
      const int mid = (low + high + 1) / 2;
      const std::wstring candidate = title.substr(0, mid) + ellipsis;
      GetTextExtentPoint32W(dc, candidate.c_str(), static_cast<int>(candidate.size()), &size);
      if (size.cx <= max_px)
        low = mid;
      else
        high = mid - 1;
    }
    title = title.substr(0, low) + ellipsis;
  }
  SelectObject(dc, previous);
  ReleaseDC(tabs_, dc);
  return title;
}

void EditorWindow::recreate_theme_brushes() {
  if (window_brush_) DeleteObject(window_brush_);
  if (panel_brush_) DeleteObject(panel_brush_);
  if (field_brush_) DeleteObject(field_brush_);
  if (banner_brush_) DeleteObject(banner_brush_);
  window_brush_ = CreateSolidBrush(dark_ ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW));
  panel_brush_ = CreateSolidBrush(dark_ ? RGB(37, 37, 38) : GetSysColor(COLOR_BTNFACE));
  field_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW));
  banner_brush_ = CreateSolidBrush(dark_ ? RGB(79, 69, 25) : RGB(255, 244, 196));
}

void EditorWindow::apply_window_theme() {
  set_preferred_app_theme(settings_.theme);
  recreate_theme_brushes();

  auto& api = dark_mode_api();
  const BOOL enabled = dark_ ? TRUE : FALSE;
  if (api.allow_dark_mode_for_window) api.allow_dark_mode_for_window(window_, enabled);
  DwmSetWindowAttribute(window_, kDwmUseImmersiveDarkMode, &enabled, sizeof(enabled));
  SetWindowTheme(window_, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);

  const std::array controls{
      toolbar_, tabs_, status_, banner_, reload_button_, keep_button_, search_panel_,
      find_text_, replace_text_, find_button_, replace_button_, replace_all_button_,
      regex_check_, case_check_, all_tabs_check_, whole_word_check_, wrap_check_,
      selection_only_check_};
  for (const HWND control : controls) {
    if (!control) continue;
    if (api.allow_dark_mode_for_window) api.allow_dark_mode_for_window(control, enabled);
    const bool text_field = control == find_text_ || control == replace_text_;
    SetWindowTheme(control,
                   dark_ ? (text_field ? L"DarkMode_CFD" : L"DarkMode_Explorer") : nullptr,
                   nullptr);
  }
  for (auto& tab : documents_) {
    switch (tab.view_kind) {
      case ViewKind::LargeText:
        LargeFileView::set_dark(tab.view, dark_);
        break;
      case ViewKind::Hex:
        HexViewWindow::set_dark(tab.view, dark_);
        break;
      case ViewKind::Text:
        configure_editor(tab.view, tab.document);
        if (tab.map)
          DocumentMap::restyle(tab.map, tab.view, settings_.font_face, dark_);
        break;
    }
  }
  if (HMENU menu = GetMenu(window_)) {
    MENUINFO info{sizeof(info)};
    info.fMask = MIM_BACKGROUND;
    info.hbrBack = panel_brush_;
    SetMenuInfo(menu, &info);
  }
  if (api.flush_menu_themes) api.flush_menu_themes();
  DrawMenuBar(window_);
  RedrawWindow(window_, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void EditorWindow::rebuild_menu() {
  HMENU root = CreateMenu();
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, IDM_FILE_NEW, tr(L"&Новый\tCtrl+N", L"&New\tCtrl+N"));
  AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, tr(L"&Открыть…\tCtrl+O", L"&Open…\tCtrl+O"));
  AppendMenuW(file, MF_STRING, IDM_FILE_SAVE, tr(L"&Сохранить\tCtrl+S", L"&Save\tCtrl+S"));
  AppendMenuW(file, MF_STRING, IDM_FILE_SAVE_AS, tr(L"Сохранить &как…", L"Save &as…"));
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, IDM_FILE_CLOSE, tr(L"&Закрыть вкладку\tCtrl+W", L"&Close tab\tCtrl+W"));
  AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, tr(L"В&ыход", L"E&xit"));
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(file), tr(L"&Файл", L"&File"));

  HMENU encoding = CreatePopupMenu();
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_UTF8, L"UTF-8");
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_UTF8_BOM, L"UTF-8 BOM");
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_UTF16LE, L"UTF-16 LE");
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_UTF16BE, L"UTF-16 BE");
  AppendMenuW(encoding, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_CP1251, L"Windows-1251");
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_CP866, L"DOS CP866");
  AppendMenuW(encoding, MF_STRING, IDM_ENCODING_CP1252, L"Windows-1252");
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(encoding),
              tr(L"Переоткрыть в &кодировке", L"Reopen with &encoding"));

  HMENU edit = CreatePopupMenu();
  AppendMenuW(edit, MF_STRING, IDM_EDIT_UNDO, tr(L"&Отменить\tCtrl+Z", L"&Undo\tCtrl+Z"));
  AppendMenuW(edit, MF_STRING, IDM_EDIT_REDO, tr(L"&Повторить\tCtrl+Y", L"&Redo\tCtrl+Y"));
  AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(edit, MF_STRING, IDM_EDIT_CUT, tr(L"Вырезать\tCtrl+X", L"Cut\tCtrl+X"));
  AppendMenuW(edit, MF_STRING, IDM_EDIT_COPY, tr(L"Копировать\tCtrl+C", L"Copy\tCtrl+C"));
  AppendMenuW(edit, MF_STRING, IDM_EDIT_PASTE, tr(L"Вставить\tCtrl+V", L"Paste\tCtrl+V"));
  AppendMenuW(edit, MF_STRING, IDM_EDIT_SELECT_ALL, tr(L"Выделить всё\tCtrl+A", L"Select all\tCtrl+A"));
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), tr(L"&Правка", L"&Edit"));

  HMENU search = CreatePopupMenu();
  AppendMenuW(search, MF_STRING, IDM_SEARCH_FIND, tr(L"&Найти\tCtrl+F", L"&Find\tCtrl+F"));
  AppendMenuW(search, MF_STRING, IDM_SEARCH_REPLACE, tr(L"&Заменить\tCtrl+H", L"&Replace\tCtrl+H"));
  AppendMenuW(search, MF_STRING, IDM_SEARCH_NEXT, tr(L"Найти далее\tF3", L"Find next\tF3"));
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(search), tr(L"&Поиск", L"&Search"));

  HMENU language = CreatePopupMenu();
  language_menu_ids_.clear();
  const auto all = languages();
  const auto append_language = [this](HMENU target, const std::string& id,
                                      const std::string& display_name) {
    language_menu_ids_.push_back(id);
    const std::wstring label = utf8_to_wide(display_name);
    AppendMenuW(target, MF_STRING,
                kLanguageFirst + static_cast<UINT>(language_menu_ids_.size() - 1),
                label.c_str());
  };
  constexpr std::array<std::string_view, 17> popular{
      "text", "json", "xml", "html", "css", "javascript", "typescript",
      "python", "cpp", "csharp", "powershell", "bsl", "onescript", "sql",
      "markdown", "shell", "yaml"};
  std::unordered_set<std::string> popular_ids;
  for (const std::string_view id : popular) {
    const auto found = std::find_if(all.begin(), all.end(),
                                    [id](const LanguageInfo& item) { return item.id == id; });
    if (found == all.end()) continue;
    append_language(language, found->id, found->display_name);
    popular_ids.insert(found->id);
  }

  struct LanguageMenuEntry {
    std::string id;
    std::string display_name;
  };
  std::vector<LanguageMenuEntry> remaining;
  std::unordered_set<std::string> represented_lexers;
  for (const auto& item : all) {
    represented_lexers.insert(item.lexer);
    if (!popular_ids.contains(item.id)) remaining.push_back({item.id, item.display_name});
  }
  const int lexer_count = GetLexerCount();
  for (int index = 0; index < lexer_count; ++index) {
    std::array<char, 128> name{};
    GetLexerName(static_cast<unsigned>(index), name.data(), static_cast<int>(name.size()));
    if (!name[0]) continue;
    if (represented_lexers.contains(name.data())) continue;
    represented_lexers.insert(name.data());
    remaining.push_back({name.data(), name.data()});
  }
  std::sort(remaining.begin(), remaining.end(), [](const auto& left, const auto& right) {
    return lowercase(utf8_to_wide(left.display_name)) < lowercase(utf8_to_wide(right.display_name));
  });
  std::map<wchar_t, std::vector<LanguageMenuEntry>> groups;
  for (auto& entry : remaining) {
    const std::wstring display = utf8_to_wide(entry.display_name);
    wchar_t group = display.empty() ? L'#' : static_cast<wchar_t>(std::towupper(display.front()));
    if ((group < L'A' || group > L'Z') && (group < L'0' || group > L'9')) group = L'#';
    groups[group].push_back(std::move(entry));
  }
  if (!groups.empty()) AppendMenuW(language, MF_SEPARATOR, 0, nullptr);
  for (auto& [letter, entries] : groups) {
    HMENU submenu = CreatePopupMenu();
    for (const auto& entry : entries) append_language(submenu, entry.id, entry.display_name);
    const wchar_t label[]{letter, L'\0'};
    AppendMenuW(language, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), label);
  }
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(language), tr(L"&Язык", L"&Language"));

  HMENU view = CreatePopupMenu();
  AppendMenuW(view, MF_STRING, IDM_VIEW_DOCUMENT_MAP,
              tr(L"Карта документа", L"Document map"));
  AppendMenuW(view, MF_STRING, IDM_VIEW_HEX, L"Hex/ASCII");
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(view),
              tr(L"&Вид", L"&View"));

  HMENU tools = CreatePopupMenu();
  AppendMenuW(tools, MF_STRING, IDM_TOOLS_FORMAT, tr(L"Форматировать\tAlt+Shift+F", L"Format\tAlt+Shift+F"));
  AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(tools, MF_STRING, IDM_TOOLS_REGISTER, tr(L"Добавить классическое ПКМ", L"Register classic context menu"));
  AppendMenuW(tools, MF_STRING, IDM_TOOLS_UNREGISTER, tr(L"Удалить классическое ПКМ", L"Unregister classic context menu"));
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), tr(L"&Инструменты", L"&Tools"));

  HMENU help = CreatePopupMenu(); AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, tr(L"О программе", L"About"));
  AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(help), tr(L"&Справка", L"&Help"));
  prepare_menu_bar(root);
  SetMenu(window_, root);
  refresh_view_menu_state();
}

void EditorWindow::refresh_view_menu_state() {
  HMENU menu = window_ ? GetMenu(window_) : nullptr;
  if (!menu) return;
  bool changed = false;
  const auto set_checked = [&](const UINT command, const bool checked) {
    const UINT state = GetMenuState(menu, command, MF_BYCOMMAND);
    if (state == static_cast<UINT>(-1) ||
        ((state & MF_CHECKED) != 0) == checked) {
      return;
    }
    CheckMenuItem(menu, command,
                  MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
    changed = true;
  };
  const auto set_enabled = [&](const UINT command, const bool enabled) {
    const UINT state = GetMenuState(menu, command, MF_BYCOMMAND);
    if (state == static_cast<UINT>(-1) ||
        ((state & (MF_DISABLED | MF_GRAYED)) == 0) == enabled) {
      return;
    }
    EnableMenuItem(menu, command,
                   MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    changed = true;
  };
  set_checked(IDM_VIEW_DOCUMENT_MAP, settings_.show_document_map);
  const Tab* tab = active_tab();
  set_checked(IDM_VIEW_HEX, tab && tab->view_kind == ViewKind::Hex);
  const bool can_switch =
      tab && tab->document.has_path() && !tab->document.dirty &&
      !tab->document.external_diverged;
  set_enabled(IDM_VIEW_HEX, can_switch);
  if (changed) DrawMenuBar(window_);
}

void EditorWindow::prepare_menu_bar(HMENU menu) {
  menu_visuals_.clear();
  const int count = GetMenuItemCount(menu);
  menu_visuals_.reserve(static_cast<std::size_t>((std::max)(count, 0)));
  for (int index = 0; index < count; ++index) {
    const int length = GetMenuStringW(menu, static_cast<UINT>(index), nullptr, 0, MF_BYPOSITION);
    auto visual = std::make_unique<MenuVisual>();
    visual->text.resize(static_cast<std::size_t>((std::max)(length, 0)) + 1, L'\0');
    if (length > 0) {
      GetMenuStringW(menu, static_cast<UINT>(index), visual->text.data(), length + 1,
                     MF_BYPOSITION);
    }
    visual->text.resize(static_cast<std::size_t>((std::max)(length, 0)));
    MENUITEMINFOW info{sizeof(info)};
    info.fMask = MIIM_FTYPE | MIIM_DATA;
    if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) continue;
    info.fType |= MFT_OWNERDRAW;
    info.dwItemData = reinterpret_cast<ULONG_PTR>(visual.get());
    if (!SetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) continue;
    menu_visuals_.push_back(std::move(visual));
  }
}

void EditorWindow::on_size() { SendMessageW(status_, WM_SIZE, 0, 0); update_layout(); }

void EditorWindow::update_layout() {
  if (!window_ || !tabs_) return;
  RECT client{}; GetClientRect(window_, &client);
  RECT status_rect{}; GetWindowRect(status_, &status_rect);
  const int status_height = status_rect.bottom - status_rect.top;
  const bool banner_visible = IsWindowVisible(banner_) != FALSE;
  const bool search_visible = IsWindowVisible(search_panel_) != FALSE;
  const bool replace_visible = IsWindowVisible(replace_text_) != FALSE;
  const int banner_height = banner_visible ? 36 : 0;
  const int bar_height = toolbar_height();
  const int top_offset = bar_height + banner_height;
  const int search_height = search_visible ? (replace_visible ? 112 : 78) : 0;
  if (toolbar_) MoveWindow(toolbar_, 0, 0, client.right, bar_height, TRUE);
  MoveWindow(tabs_, 0, top_offset, client.right, client.bottom - status_height - top_offset - search_height, TRUE);
  RECT content{0, 0, client.right, client.bottom - status_height - top_offset - search_height};
  TabCtrl_AdjustRect(tabs_, FALSE, &content);
  const int active = active_index();
  const int available_width = std::max(0L, content.right - content.left);
  const int desired_map_width = MulDiv(140, dpi_, 96);
  const int minimum_editor_width = MulDiv(160, dpi_, 96);
  for (int index = 0; index < static_cast<int>(documents_.size()); ++index) {
    Tab& tab = documents_[index];
    const bool map_requested =
        settings_.show_document_map && tab.view_kind == ViewKind::Text &&
        tab.map;
    const int map_width = map_requested
        ? std::min(desired_map_width,
                   std::max(0, available_width - minimum_editor_width))
        : 0;
    MoveWindow(tab.view, content.left, content.top + top_offset,
               available_width - map_width, content.bottom - content.top,
               TRUE);
    if (tab.map) {
      MoveWindow(tab.map, content.right - map_width, content.top + top_offset,
                 map_width, content.bottom - content.top, TRUE);
      if (map_width > 0) DocumentMap::sync(tab.map, tab.view);
      ShowWindow(tab.map,
                 index == active && map_requested && map_width > 0
                     ? SW_SHOW
                     : SW_HIDE);
    }
  }
  if (Tab* tab = active_tab(); tab && tab->view) {
    SetWindowPos(tab->view, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (tab->map && IsWindowVisible(tab->map)) {
      SetWindowPos(tab->map, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }
  if (banner_visible) {
    SetWindowPos(banner_, HWND_BOTTOM, 0, bar_height, client.right, 36, SWP_NOACTIVATE);
    SetWindowPos(reload_button_, HWND_TOP, client.right - 260, bar_height + 5, 120, 26,
                 SWP_NOACTIVATE);
    SetWindowPos(keep_button_, HWND_TOP, client.right - 132, bar_height + 5, 120, 26,
                 SWP_NOACTIVATE);
  }
  if (search_visible) {
    const int top = client.bottom - status_height - search_height;
    MoveWindow(search_panel_, 0, top, client.right, search_height, TRUE);
    MoveWindow(find_text_, 10, top + 8, static_cast<int>(std::max<LONG>(160, client.right - 145)), 24, TRUE);
    MoveWindow(find_button_, client.right - 125, top + 7, 115, 26, TRUE);
    MoveWindow(regex_check_, 10, top + 42, 42, 24, TRUE);
    MoveWindow(case_check_, 58, top + 42, 42, 24, TRUE);
    MoveWindow(whole_word_check_, 106, top + 42, 108, 24, TRUE);
    MoveWindow(wrap_check_, 220, top + 42, 92, 24, TRUE);
    MoveWindow(selection_only_check_, 318, top + 42, 120, 24, TRUE);
    MoveWindow(all_tabs_check_, 444, top + 42, 120, 24, TRUE);
    if (replace_visible) {
      MoveWindow(replace_text_, 10, top + 76, static_cast<int>(std::max<LONG>(160, client.right - 270)), 24, TRUE);
      MoveWindow(replace_button_, client.right - 250, top + 75, 115, 26, TRUE);
      MoveWindow(replace_all_button_, client.right - 125, top + 75, 115, 26, TRUE);
    }
  }
}

LRESULT CALLBACK EditorWindow::tabs_subclass(HWND window, const UINT message,
                                              const WPARAM wparam, const LPARAM lparam,
                                              const UINT_PTR id, const DWORD_PTR data) {
  auto* self = reinterpret_cast<EditorWindow*>(data);
  const auto close_rect_for = [window](const int index) {
    RECT item{};
    if (!TabCtrl_GetItemRect(window, index, &item)) return RECT{};
    const int item_height = static_cast<int>(item.bottom - item.top);
    const int size = (std::min)(16, (std::max)(10, item_height - 8));
    const int right = item.right - 8;
    const int top = item.top + ((item.bottom - item.top) - size) / 2;
    return RECT{right - size, top, right, top + size};
  };
  const auto close_at = [window, &close_rect_for](const POINT point) {
    const int count = TabCtrl_GetItemCount(window);
    for (int index = 0; index < count; ++index) {
      RECT close = close_rect_for(index);
      if (PtInRect(&close, point)) return index;
    }
    return -1;
  };
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, tabs_subclass, id);
  } else if (self && message == WM_ERASEBKGND) {
    return 1;
  } else if (self && message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    SetDCBrushColor(dc, self->dark_ ? RGB(30, 30, 30) : GetSysColor(COLOR_WINDOW));
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT header = client;
    header.bottom = 28;
    if (TabCtrl_GetItemCount(window) > 0) {
      RECT first{};
      if (TabCtrl_GetItemRect(window, 0, &first)) header.bottom = first.bottom + 1;
    }
    const COLORREF header_background = self->dark_ ? RGB(37, 37, 38) : GetSysColor(COLOR_BTNFACE);
    SetDCBrushColor(dc, header_background);
    FillRect(dc, &header, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));

    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    const HGDIOBJ previous_font = SelectObject(
        dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
    const int previous_mode = SetBkMode(dc, TRANSPARENT);
    const int selected = TabCtrl_GetCurSel(window);
    const int count = TabCtrl_GetItemCount(window);
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(window, &cursor);
    for (int index = 0; index < count; ++index) {
      RECT item_rect{};
      if (!TabCtrl_GetItemRect(window, index, &item_rect)) continue;
      std::array<wchar_t, 512> title{};
      TCITEMW item{};
      item.mask = TCIF_TEXT;
      item.pszText = title.data();
      item.cchTextMax = static_cast<int>(title.size());
      TabCtrl_GetItem(window, index, &item);

      const bool active = index == selected;
      const COLORREF item_background = self->dark_
          ? (active ? RGB(30, 30, 30) : RGB(45, 45, 48))
          : (active ? GetSysColor(COLOR_WINDOW) : GetSysColor(COLOR_BTNFACE));
      SetDCBrushColor(dc, item_background);
      FillRect(dc, &item_rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      if (active) {
        RECT accent{item_rect.left, item_rect.top, item_rect.right,
                    item_rect.top + MulDiv(3, self->dpi_, 96)};
        SetDCBrushColor(dc, RGB(254, 138, 0));  // the leaf orange from the logo
        FillRect(dc, &accent, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      }
      SetTextColor(dc, self->dark_
                           ? (active ? RGB(245, 245, 245) : RGB(190, 190, 190))
                           : GetSysColor(COLOR_BTNTEXT));
      RECT close_rect = close_rect_for(index);
      const bool close_hovered = PtInRect(&close_rect, cursor) != FALSE;
      const bool close_pressed = self->pressed_close_tab_ == index;
      if (close_hovered || close_pressed) {
        SetDCBrushColor(dc, self->dark_
                               ? (close_pressed ? RGB(90, 55, 55) : RGB(63, 63, 70))
                               : (close_pressed ? RGB(225, 200, 200) : RGB(225, 225, 225)));
        RECT close_background = close_rect;
        InflateRect(&close_background, 3, 3);
        FillRect(dc, &close_background, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
      }
      RECT text_rect = item_rect;
      text_rect.left += 10;
      text_rect.right = close_rect.left - 7;
      DrawTextW(dc, title.data(), -1, &text_rect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      const HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(DC_PEN));
      SetDCPenColor(dc, self->dark_ ? RGB(205, 205, 205) : RGB(75, 75, 75));
      MoveToEx(dc, close_rect.left + 3, close_rect.top + 3, nullptr);
      LineTo(dc, close_rect.right - 2, close_rect.bottom - 2);
      MoveToEx(dc, close_rect.right - 3, close_rect.top + 3, nullptr);
      LineTo(dc, close_rect.left + 2, close_rect.bottom - 2);
      SelectObject(dc, previous_pen);
    }
    SetDCBrushColor(dc, self->dark_ ? RGB(63, 63, 70) : GetSysColor(COLOR_3DSHADOW));
    RECT divider{header.left, header.bottom - 1, header.right, header.bottom};
    if (count > 0) {
      RECT last{};
      if (TabCtrl_GetItemRect(window, count - 1, &last)) {
        divider.top = last.bottom - 1;
        divider.bottom = last.bottom;
      }
    }
    FillRect(dc, &divider, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(dc, previous_mode);
    SelectObject(dc, previous_font);
    EndPaint(window, &paint);
    return 0;
  } else if (self && message == WM_LBUTTONDOWN) {
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const int close = close_at(point);
    if (close >= 0) {
      self->pressed_close_tab_ = close;
      SetCapture(window);
      InvalidateRect(window, nullptr, FALSE);
      return 0;
    }
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    InvalidateRect(window, nullptr, FALSE);
    return result;
  } else if (self && message == WM_LBUTTONUP && self->pressed_close_tab_ >= 0) {
    const int pressed = std::exchange(self->pressed_close_tab_, -1);
    if (GetCapture() == window) ReleaseCapture();
    const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (close_at(point) == pressed) self->close_tab(pressed);
    InvalidateRect(window, nullptr, FALSE);
    return 0;
  } else if (self && message == WM_MBUTTONUP) {
    TCHITTESTINFO hit{{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, 0};
    const int index = TabCtrl_HitTest(window, &hit);
    if (index >= 0) self->close_tab(index);
    return 0;
  } else if (self && message == WM_CAPTURECHANGED) {
    self->pressed_close_tab_ = -1;
    InvalidateRect(window, nullptr, FALSE);
  } else if (self && message == WM_MOUSEMOVE) {
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
    InvalidateRect(window, nullptr, FALSE);
    if (self->pressed_close_tab_ >= 0) return 0;
    const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    return result;
  } else if (self && message == WM_MOUSELEAVE) {
    InvalidateRect(window, nullptr, FALSE);
    return 0;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK EditorWindow::status_subclass(HWND window, const UINT message,
                                                const WPARAM wparam, const LPARAM lparam,
                                                const UINT_PTR id, const DWORD_PTR data) {
  auto* self = reinterpret_cast<EditorWindow*>(data);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, status_subclass, id);
  } else if (self && self->dark_ && message == WM_ERASEBKGND) {
    return 1;
  } else if (self && self->dark_ && message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    SetDCBrushColor(dc, RGB(37, 37, 38));
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    const HGDIOBJ previous_font = SelectObject(
        dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
    const int previous_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF previous_text = SetTextColor(dc, RGB(210, 210, 210));
    const int part_count = static_cast<int>(SendMessageW(window, SB_GETPARTS, 0, 0));
    for (int index = 0; index < part_count; ++index) {
      RECT part{};
      if (!SendMessageW(window, SB_GETRECT, index, reinterpret_cast<LPARAM>(&part))) continue;
      const LRESULT length_and_type = SendMessageW(window, SB_GETTEXTLENGTHW, index, 0);
      const std::size_t length = LOWORD(length_and_type);
      std::wstring text(length + 1, L'\0');
      if (length > 0) SendMessageW(window, SB_GETTEXTW, index, pointer_param(text.data()));
      text.resize(length);
      RECT text_rect = part;
      text_rect.left += 7;
      text_rect.right -= 5;
      DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      if (index + 1 < part_count) {
        SetDCPenColor(dc, RGB(70, 70, 70));
        const HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(DC_PEN));
        MoveToEx(dc, part.right - 1, part.top + 3, nullptr);
        LineTo(dc, part.right - 1, part.bottom - 3);
        SelectObject(dc, previous_pen);
      }
    }
    SetTextColor(dc, previous_text);
    SetBkMode(dc, previous_mode);
    SelectObject(dc, previous_font);
    EndPaint(window, &paint);
    return 0;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

EditorWindow::Tab* EditorWindow::active_tab() {
  const int index = active_index(); return index >= 0 ? &documents_[index] : nullptr;
}
const EditorWindow::Tab* EditorWindow::active_tab() const {
  const int index = active_index(); return index >= 0 ? &documents_[index] : nullptr;
}
int EditorWindow::active_index() const {
  const int selected = tabs_ ? TabCtrl_GetCurSel(tabs_) : -1;
  return selected >= 0 && selected < static_cast<int>(documents_.size()) ? selected : -1;
}

void EditorWindow::activate_tab(const int index) {
  if (index < 0 || index >= static_cast<int>(documents_.size())) return;
  TabCtrl_SetCurSel(tabs_, index);
  for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
    ShowWindow(documents_[i].view, i == index ? SW_SHOW : SW_HIDE);
    if (documents_[i].map) ShowWindow(documents_[i].map, SW_HIDE);
  }
  SetFocus(documents_[index].view);
  refresh_view_menu_state();
  update_ui();
  update_layout();
}

HWND EditorWindow::create_editor() {
  HWND editor = CreateWindowExW(0, L"Scintilla", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
                                0, 0, 0, 0,
                                window_, reinterpret_cast<HMENU>(IDC_EDITOR), instance_, nullptr);
  SetWindowSubclass(editor, editor_subclass, 1, reinterpret_cast<DWORD_PTR>(this));
  return editor;
}

void EditorWindow::destroy_tab_views(Tab& tab) {
  if (tab.map) {
    DestroyWindow(tab.map);
    tab.map = nullptr;
  }
  if (tab.view) {
    DestroyWindow(tab.view);
    tab.view = nullptr;
  }
}

bool EditorWindow::create_tab_views(Tab& tab) {
  destroy_tab_views(tab);
  switch (tab.view_kind) {
    case ViewKind::Text:
      tab.view = create_editor();
      if (!tab.view) return false;
      configure_editor(tab.view, tab.document);
      set_editor_text(tab.view, tab.document.text, true);
      tab.map = DocumentMap::create(window_, IDC_DOCUMENT_MAP, instance_);
      if (!tab.map) {
        destroy_tab_views(tab);
        return false;
      }
      DocumentMap::attach(tab.map, tab.view);
      DocumentMap::restyle(tab.map, tab.view, settings_.font_face, dark_);
      DocumentMap::sync(tab.map, tab.view);
      return true;
    case ViewKind::LargeText:
      tab.view = LargeFileView::create(window_, IDC_EDITOR);
      if (!tab.view) return false;
      SendMessageW(tab.view, WM_SETFONT, reinterpret_cast<WPARAM>(editor_font_),
                   TRUE);
      LargeFileView::set_dark(tab.view, dark_);
      if (!LargeFileView::open(tab.view, tab.document.path)) {
        destroy_tab_views(tab);
        return false;
      }
      return true;
    case ViewKind::Hex:
      tab.view = HexViewWindow::create(window_, IDC_EDITOR);
      if (!tab.view) return false;
      SendMessageW(tab.view, WM_SETFONT, reinterpret_cast<WPARAM>(editor_font_),
                   TRUE);
      HexViewWindow::set_dark(tab.view, dark_);
      if (!HexViewWindow::open(tab.view, tab.document.path)) {
        destroy_tab_views(tab);
        return false;
      }
      return true;
  }
  return false;
}

bool EditorWindow::switch_tab_view(Tab& tab, const ViewKind requested) {
  if (!tab.document.has_path() || tab.document.dirty ||
      tab.document.external_diverged) {
    return false;
  }

  ViewKind target = requested;
  std::optional<Document> replacement;
  if (requested == ViewKind::Text && tab.view_kind == ViewKind::Hex) {
    const Encoding* forced =
        tab.document.large_file ? nullptr : &tab.document.encoding;
    LoadDocumentResult loaded = load_document(
        tab.document.path, settings_.large_file_threshold, forced);
    if (!loaded.ok) {
      MessageBoxW(
          window_,
          (tr(L"Не удалось открыть файл:\n", L"Unable to open file:\n") +
           win32_error_message(loaded.error))
              .c_str(),
          LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
      return false;
    }
    target = loaded.document.large_file ? ViewKind::LargeText : ViewKind::Text;
    replacement = std::move(loaded.document);
  }
  if (target == tab.view_kind) return true;

  search_thread_.request_stop();
  ++search_generation_;
  const ViewKind previous_kind = tab.view_kind;
  const HWND previous_view = tab.view;
  const HWND previous_map = tab.map;
  std::optional<Document> previous_document;
  if (replacement) {
    previous_document = std::move(tab.document);
    tab.document = std::move(*replacement);
  }
  tab.view_kind = target;
  tab.view = nullptr;
  tab.map = nullptr;
  if (!create_tab_views(tab)) {
    tab.view_kind = previous_kind;
    tab.view = previous_view;
    tab.map = previous_map;
    if (previous_document) tab.document = std::move(*previous_document);
    MessageBoxW(window_,
                tr(L"Не удалось создать представление файла.",
                   L"Unable to create the file view."),
                LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
    return false;
  }

  if (previous_map) DestroyWindow(previous_map);
  if (previous_view) DestroyWindow(previous_view);
  if (target == ViewKind::Hex) tab.document.text.clear();
  tab.snippet_fields.clear();
  tab.snippet_index = 0;
  refresh_view_menu_state();
  update_ui();
  update_layout();
  SetFocus(tab.view);
  return true;
}

void EditorWindow::add_empty_tab() {
  Tab tab;
  tab.document.title = tr(L"Без имени", L"Untitled");
  documents_.push_back(std::move(tab));
  if (!create_tab_views(documents_.back())) {
    documents_.pop_back();
    return;
  }
  TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = documents_.back().document.title.data();
  TabCtrl_InsertItem(tabs_, static_cast<int>(documents_.size() - 1), &item);
  activate_tab(static_cast<int>(documents_.size() - 1));
}

void EditorWindow::configure_editor(HWND editor, const Document& document) {
  auto& api = dark_mode_api();
  if (api.allow_dark_mode_for_window) api.allow_dark_mode_for_window(editor, dark_ ? TRUE : FALSE);
  SetWindowTheme(editor, dark_ ? L"DarkMode_Explorer" : nullptr, nullptr);
  sci(editor, SCI_SETCODEPAGE, SC_CP_UTF8);
  sci(editor, SCI_SETWRAPMODE, SC_WRAP_NONE);
  sci(editor, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
  sci(editor, SCI_SETMARGINWIDTHN, 0, 48);
  sci(editor, SCI_SETMARGINTYPEN, 1, SC_MARGIN_SYMBOL);
  sci(editor, SCI_SETMARGINMASKN, 1, SC_MASK_FOLDERS);
  sci(editor, SCI_SETMARGINWIDTHN, 1, 14);
  sci(editor, SCI_SETMARGINSENSITIVEN, 1, TRUE);
  sci(editor, SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
  sci(editor, SCI_SETCARETLINEVISIBLE, TRUE);
  sci(editor, SCI_SETMULTIPLESELECTION, TRUE);
  sci(editor, SCI_SETADDITIONALSELECTIONTYPING, TRUE);
  sci(editor, SCI_SETTABWIDTH, settings_.indent_size);
  sci(editor, SCI_SETUSETABS, settings_.indent_with_tabs);
  sci(editor, SCI_SETEOLMODE, document.eol == EolMode::Lf ? SC_EOL_LF : document.eol == EolMode::Cr ? SC_EOL_CR : SC_EOL_CRLF);
  const std::string face = settings_.font_face;
  sci(editor, SCI_STYLESETFONT, STYLE_DEFAULT, pointer_param(face.c_str()));
  sci(editor, SCI_STYLESETSIZE, STYLE_DEFAULT, settings_.font_size);
  const COLORREF foreground = dark_ ? RGB(220, 220, 220) : RGB(32, 32, 32);
  const COLORREF background = dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255);
  const COLORREF gutter_foreground = dark_ ? RGB(145, 145, 145) : RGB(105, 105, 105);
  const COLORREF gutter_background = dark_ ? RGB(37, 37, 38) : RGB(245, 245, 245);
  sci(editor, SCI_STYLESETFORE, STYLE_DEFAULT, foreground);
  sci(editor, SCI_STYLESETBACK, STYLE_DEFAULT, background);
  sci(editor, SCI_STYLECLEARALL);
  sci(editor, SCI_STYLESETFORE, STYLE_LINENUMBER, gutter_foreground);
  sci(editor, SCI_STYLESETBACK, STYLE_LINENUMBER, gutter_background);
  sci(editor, SCI_STYLESETFORE, STYLE_CONTROLCHAR, gutter_foreground);
  sci(editor, SCI_STYLESETBACK, STYLE_CONTROLCHAR, background);
  sci(editor, SCI_SETFOLDMARGINCOLOUR, TRUE, gutter_background);
  sci(editor, SCI_SETFOLDMARGINHICOLOUR, TRUE,
      dark_ ? RGB(48, 48, 48) : RGB(232, 232, 232));
  sci(editor, SCI_SETWHITESPACEFORE, TRUE,
      dark_ ? RGB(90, 90, 90) : RGB(180, 180, 180));
  sci(editor, SCI_SETCARETFORE, dark_ ? RGB(255, 255, 255) : RGB(0, 0, 0));
  sci(editor, SCI_SETCARETLINEBACK, dark_ ? RGB(43, 43, 43) : RGB(245, 248, 252));
  sci(editor, SCI_SETSELFORE, TRUE, dark_ ? RGB(255, 255, 255) : RGB(0, 0, 0));
  sci(editor, SCI_SETSELBACK, TRUE, dark_ ? RGB(62, 95, 135) : RGB(190, 215, 245));
  const auto found = std::find_if(documents_.begin(), documents_.end(),
                                  [editor](const Tab& tab) { return tab.view == editor; });
  if (found != documents_.end()) apply_language(*found, document.language);
}

void EditorWindow::apply_language(Tab& tab, const std::string_view language) {
  const LanguageInfo* info = language_by_id(language);
  const std::string lexer_name = info ? info->lexer : std::string(language);
  tab.document.language = info ? info->id : lexer_name;
  Scintilla::ILexer5* lexer = nullptr;
  if (lexer_name == "bsl") lexer = create_bsl_lexer();
  else if (lexer_name == "hypertext") lexer = create_html_css_lexer();
  else if (lexer_name != "null") lexer = CreateLexer(lexer_name.c_str());
  if (!lexer && lexer_name != "null") {
    tab.document.language = "text";
  }
  sci(tab.view, SCI_SETILEXER, 0, reinterpret_cast<LPARAM>(lexer));
  const COLORREF normal = dark_ ? RGB(220, 220, 220) : RGB(32, 32, 32);
  const COLORREF background = dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255);
  const COLORREF comment = dark_ ? RGB(106, 153, 85) : RGB(0, 128, 0);
  const COLORREF keyword = dark_ ? RGB(86, 156, 214) : RGB(0, 0, 190);
  const COLORREF string = dark_ ? RGB(206, 145, 120) : RGB(163, 21, 21);
  const COLORREF number = dark_ ? RGB(181, 206, 168) : RGB(0, 120, 100);
  const COLORREF attribute = dark_ ? RGB(156, 220, 254) : RGB(0, 92, 128);
  const COLORREF type = dark_ ? RGB(78, 201, 176) : RGB(38, 127, 153);
  const COLORREF callable = dark_ ? RGB(220, 220, 170) : RGB(121, 94, 38);
  const COLORREF preprocessor = dark_ ? RGB(197, 134, 192) : RGB(128, 0, 128);
  const COLORREF error = dark_ ? RGB(244, 71, 71) : RGB(190, 0, 0);
  for (int style = 0; style < 256; ++style) {
    sci(tab.view, SCI_STYLESETFORE, style, normal);
    sci(tab.view, SCI_STYLESETBACK, style, background);
    sci(tab.view, SCI_STYLESETBOLD, style, FALSE);
    sci(tab.view, SCI_STYLESETITALIC, style, FALSE);
  }
  sci(tab.view, SCI_STYLESETFORE, STYLE_LINENUMBER,
      dark_ ? RGB(145, 145, 145) : RGB(105, 105, 105));
  sci(tab.view, SCI_STYLESETBACK, STYLE_LINENUMBER,
      dark_ ? RGB(37, 37, 38) : RGB(245, 245, 245));
  const auto set_fore = [&](const int style, const COLORREF colour, const bool bold = false) {
    sci(tab.view, SCI_STYLESETFORE, style, colour);
    if (bold) sci(tab.view, SCI_STYLESETBOLD, style, TRUE);
  };
  const auto configure_css_styles = [&](const int base) {
    set_fore(base + SCE_CSS_TAG, callable);
    set_fore(base + SCE_CSS_CLASS, callable);
    set_fore(base + SCE_CSS_PSEUDOCLASS, attribute);
    set_fore(base + SCE_CSS_UNKNOWN_PSEUDOCLASS, attribute);
    set_fore(base + SCE_CSS_IDENTIFIER, attribute);
    set_fore(base + SCE_CSS_UNKNOWN_IDENTIFIER, attribute);
    set_fore(base + SCE_CSS_VALUE, string);
    set_fore(base + SCE_CSS_COMMENT, comment);
    set_fore(base + SCE_CSS_ID, callable);
    set_fore(base + SCE_CSS_IMPORTANT, keyword, true);
    set_fore(base + SCE_CSS_DIRECTIVE, preprocessor);
    set_fore(base + SCE_CSS_DOUBLESTRING, string);
    set_fore(base + SCE_CSS_SINGLESTRING, string);
    set_fore(base + SCE_CSS_IDENTIFIER2, attribute);
    set_fore(base + SCE_CSS_ATTRIBUTE, attribute);
    set_fore(base + SCE_CSS_IDENTIFIER3, attribute);
    set_fore(base + SCE_CSS_PSEUDOELEMENT, attribute);
    set_fore(base + SCE_CSS_EXTENDED_IDENTIFIER, attribute);
    set_fore(base + SCE_CSS_EXTENDED_PSEUDOCLASS, attribute);
    set_fore(base + SCE_CSS_EXTENDED_PSEUDOELEMENT, attribute);
    set_fore(base + SCE_CSS_GROUP_RULE, preprocessor);
    set_fore(base + SCE_CSS_VARIABLE, attribute);
  };

  static constexpr char common_keywords[] =
      "if else for while do switch case default break continue return class struct enum namespace "
      "public private protected static const constexpr auto void int string true false null nullptr "
      "function import export from async await try catch finally throw new this let var def in and or not";
  static constexpr char javascript_keywords[] =
      "as async await break case catch class const continue debugger default delete do else export extends "
      "false finally for from function get if import in instanceof let new null of return set static super "
      "switch this throw true try typeof undefined var void while with yield";
  static constexpr char bsl_keywords[] =
      "break continue do each else elseif elsif enddo endfunction endif endprocedure endtry except export false "
      "for function goto if in new not null procedure raise return then to true try undefined val var while "
      "асинх вызватьисключение возврат для добавитьобработчик ждать если значение из иначе иначеесли исключение "
      "истина каждого конецесли конецпопытки конецпроцедуры конецфункции конеццикла ложь не новый перем перейти "
      "по пока попытка прервать продолжить процедура удалитьобработчик тогда функция цикл экспорт выполнить и или";
  static constexpr char bsl_types[] =
      "array boolean date false map null number string structure true undefined valuetable valuelist "
      "булево дата ложь массив неопределено null истина списокзначений соответствие строка структура "
      "таблицазначений число этотобъект этаформа";
  static constexpr char bsl_functions[] =
      "assert attachscript boolean date eval execute exit find format getscriptoption importscript loadscript "
      "message number sleep string type typeof valueisfilled "
      "ввестиесли ввестистроку выполнить заполненозначение найти сообщить строка число дата тип типзнч формат";

  if (lexer_name == "hypertext") {
    set_fore(SCE_H_TAG, keyword); set_fore(SCE_H_TAGUNKNOWN, keyword);
    set_fore(SCE_H_ATTRIBUTE, attribute); set_fore(SCE_H_ATTRIBUTEUNKNOWN, attribute);
    set_fore(SCE_H_NUMBER, number);
    set_fore(SCE_H_DOUBLESTRING, string); set_fore(SCE_H_SINGLESTRING, string);
    set_fore(SCE_H_COMMENT, comment); set_fore(SCE_H_XCCOMMENT, comment);
    set_fore(SCE_H_ENTITY, number); set_fore(SCE_H_TAGEND, keyword);
    set_fore(SCE_H_XMLSTART, preprocessor); set_fore(SCE_H_XMLEND, preprocessor);
    set_fore(SCE_H_CDATA, string); set_fore(SCE_H_QUESTION, preprocessor);
    set_fore(SCE_H_VALUE, string);
    set_fore(SCE_H_SGML_COMMAND, keyword); set_fore(SCE_H_SGML_1ST_PARAM, attribute);
    set_fore(SCE_H_SGML_DOUBLESTRING, string); set_fore(SCE_H_SGML_SIMPLESTRING, string);
    set_fore(SCE_H_SGML_ERROR, error); set_fore(SCE_H_SGML_SPECIAL, number);
    set_fore(SCE_H_SGML_ENTITY, number); set_fore(SCE_H_SGML_COMMENT, comment);
    for (const int offset : {SCE_HJ_START, SCE_HJA_START}) {
      set_fore(offset + (SCE_HJ_COMMENT - SCE_HJ_START), comment);
      set_fore(offset + (SCE_HJ_COMMENTLINE - SCE_HJ_START), comment);
      set_fore(offset + (SCE_HJ_COMMENTDOC - SCE_HJ_START), comment);
      set_fore(offset + (SCE_HJ_NUMBER - SCE_HJ_START), number);
      set_fore(offset + (SCE_HJ_KEYWORD - SCE_HJ_START), keyword, true);
      set_fore(offset + (SCE_HJ_DOUBLESTRING - SCE_HJ_START), string);
      set_fore(offset + (SCE_HJ_SINGLESTRING - SCE_HJ_START), string);
      set_fore(offset + (SCE_HJ_REGEX - SCE_HJ_START), string);
      set_fore(offset + (SCE_HJ_TEMPLATELITERAL - SCE_HJ_START), string);
    }
    configure_css_styles(kEmbeddedCssStyleBase);
    sci(tab.view, SCI_SETKEYWORDS, 1, pointer_param(javascript_keywords));
  } else if (lexer_name == "css") {
    configure_css_styles(0);
  } else if (lexer_name == "bsl") {
    set_fore(BslComment, comment);
    set_fore(BslNumber, number);
    set_fore(BslString, string);
    set_fore(BslDate, string);
    set_fore(BslKeyword, keyword, true);
    set_fore(BslPreprocessor, preprocessor, true);
    set_fore(BslAnnotation, callable, true);
    set_fore(BslType, type);
    set_fore(BslFunction, callable);
    sci(tab.view, SCI_SETKEYWORDS, 0, pointer_param(bsl_keywords));
    sci(tab.view, SCI_SETKEYWORDS, 1, pointer_param(bsl_types));
    sci(tab.view, SCI_SETKEYWORDS, 2, pointer_param(bsl_functions));
  } else {
    set_fore(1, comment); set_fore(2, comment);
    set_fore(3, number); set_fore(4, keyword);
    set_fore(5, keyword, true);
    set_fore(6, string); set_fore(7, string);
    set_fore(9, keyword); set_fore(10, string);
    sci(tab.view, SCI_SETKEYWORDS, 0, pointer_param(common_keywords));
  }
  sci(tab.view, SCI_SETPROPERTY, pointer_param("fold"), pointer_param("1"));
  sci(tab.view, SCI_COLOURISE, 0, -1);
  if (tab.map) {
    DocumentMap::restyle(tab.map, tab.view, settings_.font_face, dark_);
    DocumentMap::sync(tab.map, tab.view);
  }
  update_ui();
}

std::string EditorWindow::editor_text(HWND editor) const {
  const auto length = static_cast<std::size_t>(sci(editor, SCI_GETTEXTLENGTH));
  std::string text(length + 1, '\0');
  sci(editor, SCI_GETTEXT, text.size(), pointer_param(text.data()));
  text.resize(length); return text;
}

void EditorWindow::set_editor_text(HWND editor, const std::string_view text, const bool save_point) {
  sci(editor, SCI_SETREADONLY, FALSE);
  sci(editor, SCI_SETTEXT, 0, pointer_param(std::string(text).c_str()));
  if (save_point) sci(editor, SCI_SETSAVEPOINT);
}

bool EditorWindow::open_file(const std::filesystem::path& input, const Encoding* forced) {
  const auto path = canonical_path(input);
  for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
    if (documents_[i].document.has_path() && lowercase(documents_[i].document.path.wstring()) == lowercase(path.wstring())) {
      activate_tab(i); return true;
    }
  }
  LoadDocumentResult loaded = load_document(path, settings_.large_file_threshold, forced);
  if (!loaded.ok) {
    MessageBoxW(window_, (tr(L"Не удалось открыть файл:\n", L"Unable to open file:\n") + win32_error_message(loaded.error)).c_str(),
                LISTOPAD_PRODUCT_NAME, MB_ICONERROR); return false;
  }
  ViewKind view_kind =
      loaded.document.large_file ? ViewKind::LargeText : ViewKind::Text;
  if (loaded.document.likely_binary) {
    const int answer = MessageBoxW(
        window_,
        tr(L"Файл похож на бинарный.\n"
           L"Да — открыть в Hex/ASCII, Нет — открыть как текст, "
           L"Отмена — не открывать.",
           L"The file appears to be binary.\n"
           L"Yes — open in Hex/ASCII, No — open as text, "
           L"Cancel — do not open."),
        LISTOPAD_PRODUCT_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    view_kind = answer == IDYES ? ViewKind::Hex : ViewKind::Text;
  }
  if (documents_.size() == 1 && !documents_[0].document.has_path() && !documents_[0].document.dirty &&
      editable(documents_[0]) && editor_text(documents_[0].view).empty()) {
    destroy_tab_views(documents_[0]);
    documents_.clear();
    TabCtrl_DeleteAllItems(tabs_);
  }
  Tab tab;
  tab.document = std::move(loaded.document);
  tab.view_kind = view_kind;
  documents_.push_back(std::move(tab));
  Tab& added = documents_.back();
  if (!create_tab_views(added)) {
    documents_.pop_back();
    if (documents_.empty()) add_empty_tab();
    MessageBoxW(window_,
                tr(L"Не удалось создать представление файла.",
                   L"Unable to create the file view."),
                LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONERROR);
    return false;
  }
  if (added.view_kind == ViewKind::Hex) added.document.text.clear();
  TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = documents_.back().document.title.data();
  TabCtrl_InsertItem(tabs_, static_cast<int>(documents_.size() - 1), &item);
  watcher_.watch(path); activate_tab(static_cast<int>(documents_.size() - 1)); return true;
}

void EditorWindow::open_request(const ipc::OpenFilesRequest& request) {
  std::optional<Encoding> forced;
  if (!request.encoding.empty()) forced = encoding_from_name(request.encoding);
  for (const auto& path : request.files) open_file(path, forced ? &*forced : nullptr);
  if (request.files.empty() && documents_.empty()) add_empty_tab();
  if (Tab* tab = active_tab(); tab && editable(*tab) && request.line > 0) {
    const auto line = static_cast<sptr_t>(request.line - 1);
    sptr_t position = sci(tab->view, SCI_POSITIONFROMLINE, line);
    if (request.column > 0) position += static_cast<sptr_t>(request.column - 1);
    sci(tab->view, SCI_GOTOPOS, position);
  }
  // Only un-minimize; an unconditional SW_RESTORE would also drop a maximized
  // window back to normal, which wrecks the restored-on-launch maximized state
  // (this runs on every startup, not just on a second instance's open request).
  // SW_RESTORE on a minimized window returns it to whichever state — maximized or
  // normal — it held before being minimized.
  if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
  SetForegroundWindow(window_);
}

bool EditorWindow::save_tab(Tab& tab, bool save_as) {
  if (!editable(tab)) return false;
  std::filesystem::path target = tab.document.path;
  if (save_as || target.empty()) { target = choose_save_file(tab); if (target.empty()) return false; }
  std::string text = editor_text(tab.view);
  Encoding output_encoding = tab.document.encoding;
  EncodingResult encoded = encode_text(text, output_encoding);
  if (!encoded.ok) {
    const int answer = MessageBoxW(
        window_,
        tr(L"Текст нельзя представить в исходной кодировке.\n"
           L"Да — сохранить этот файл как UTF-8, Нет — выбрать другой файл и сохранить как UTF-8, Отмена — не сохранять.",
           L"The text cannot be represented in the original encoding.\n"
           L"Yes — save this file as UTF-8, No — choose another file and save as UTF-8, Cancel — do not save."),
        LISTOPAD_PRODUCT_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    if (answer == IDNO) {
      target = choose_save_file(tab);
      if (target.empty()) return false;
      save_as = true;
    }
    output_encoding = {EncodingKind::Utf8, CP_UTF8, false};
    encoded = encode_text(text, output_encoding);
    if (!encoded.ok) return false;
  }
  bool overwrite = false;
  FileFingerprint expected = save_as ? fingerprint_file(target) : tab.document.fingerprint;
  if (tab.document.external_diverged && !save_as) {
    const int answer = MessageBoxW(window_, tr(L"Файл изменён другой программой.\nДа — перезаписать, Нет — сохранить как, Отмена — не сохранять.",
                                                L"The file was changed by another program.\nYes — overwrite, No — Save As, Cancel — do not save."),
                                   LISTOPAD_PRODUCT_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
    if (answer == IDCANCEL) return false;
    if (answer == IDNO) return save_tab(tab, true);
    overwrite = true; expected = fingerprint_file(target);
  }
  SaveFileResult saved = atomic_save(target, encoded.bytes, expected, overwrite || save_as);
  if (saved.status == SaveStatus::AccessDenied) saved = elevated_.save(target, encoded.bytes, expected);
  if (saved.status == SaveStatus::Conflict) {
    tab.document.external_diverged = true; tab.external_notice_pending = true; update_ui();
    MessageBoxW(window_, tr(L"Файл снова изменился перед сохранением.", L"The file changed again before it could be saved."),
                LISTOPAD_PRODUCT_NAME, MB_ICONWARNING); return false;
  }
  if (saved.status != SaveStatus::Saved) {
    MessageBoxW(window_, (tr(L"Не удалось сохранить файл:\n", L"Unable to save the file:\n") + win32_error_message(saved.error)).c_str(),
                LISTOPAD_PRODUCT_NAME, MB_ICONERROR); return false;
  }
  tab.document.path = canonical_path(target); tab.document.title = target.filename().wstring();
  tab.document.encoding = output_encoding;
  tab.document.fingerprint = saved.fingerprint; tab.document.dirty = false;
  tab.document.external_diverged = false; tab.external_notice_pending = false;
  tab.document.text = std::move(text); sci(tab.view, SCI_SETSAVEPOINT);
  watcher_.watch(tab.document.path); apply_language(tab, detect_language(tab.document.path).id);
  update_ui(); return true;
}

void EditorWindow::reopen_active(const Encoding& encoding) {
  Tab* tab = active_tab();
  if (!tab || !tab->document.has_path() || !editable(*tab)) return;
  if (tab->document.dirty &&
      MessageBoxW(window_, tr(L"Отбросить локальные изменения и переоткрыть файл?",
                              L"Discard local changes and reopen the file?"),
                  LISTOPAD_PRODUCT_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) return;
  auto loaded = load_document(tab->document.path, settings_.large_file_threshold, &encoding);
  if (!loaded.ok || loaded.document.large_file) {
    MessageBoxW(window_, tr(L"Не удалось переоткрыть файл в выбранной кодировке.",
                            L"Unable to reopen the file with the selected encoding."),
                LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONERROR);
    return;
  }
  tab->document = std::move(loaded.document);
  tab->external_notice_pending = false;
  configure_editor(tab->view, tab->document);
  set_editor_text(tab->view, tab->document.text, true);
  update_ui();
}

bool EditorWindow::confirm_close(Tab& tab) {
  if (!tab.document.dirty) return true;
  const int answer = MessageBoxW(window_, (tr(L"Сохранить изменения в «", L"Save changes to “") + tab.document.title + L"»?").c_str(),
                                 LISTOPAD_PRODUCT_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
  if (answer == IDCANCEL) return false;
  return answer == IDNO || save_tab(tab);
}

bool EditorWindow::close_tab(const int index) {
  if (index < 0 || index >= static_cast<int>(documents_.size()) || !confirm_close(documents_[index])) return false;
  search_thread_.request_stop();
  ++search_generation_;
  destroy_tab_views(documents_[index]);
  documents_.erase(documents_.begin() + index);
  TabCtrl_DeleteItem(tabs_, index);
  if (documents_.empty()) add_empty_tab(); else activate_tab(std::min(index, static_cast<int>(documents_.size()) - 1));
  return true;
}

void EditorWindow::handle_external_change(const std::filesystem::path& path) {
  for (auto& tab : documents_) {
    if (!tab.document.has_path() || lowercase(tab.document.path.wstring()) != lowercase(path.wstring())) continue;
    const FileFingerprint current = fingerprint_file(path);
    if (current == tab.document.fingerprint) continue;
    tab.document.external_diverged = true; tab.external_notice_pending = true;
  }
  update_ui();
}

void EditorWindow::reload_active() {
  Tab* tab = active_tab(); if (!tab || !tab->document.has_path()) return;
  if (tab->document.dirty && MessageBoxW(window_, tr(L"Отбросить локальные изменения?", L"Discard local changes?"),
                                         LISTOPAD_PRODUCT_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) return;
  switch (tab->view_kind) {
    case ViewKind::LargeText:
      if (!LargeFileView::open(tab->view, tab->document.path)) return;
      tab->document.fingerprint = fingerprint_file(tab->document.path);
      break;
    case ViewKind::Hex:
      if (!HexViewWindow::open(tab->view, tab->document.path)) return;
      tab->document.fingerprint = fingerprint_file(tab->document.path);
      break;
    case ViewKind::Text: {
      auto loaded = load_document(tab->document.path,
                                  settings_.large_file_threshold,
                                  &tab->document.encoding);
      if (!loaded.ok || loaded.document.large_file) return;
      tab->document = std::move(loaded.document);
      configure_editor(tab->view, tab->document);
      set_editor_text(tab->view, tab->document.text, true);
      if (tab->map) {
        DocumentMap::attach(tab->map, tab->view);
        DocumentMap::restyle(tab->map, tab->view, settings_.font_face, dark_);
        DocumentMap::sync(tab->map, tab->view);
      }
      break;
    }
  }
  tab->external_notice_pending = false;
  tab->document.external_diverged = false;
  refresh_view_menu_state();
  update_ui();
}

void EditorWindow::keep_external_active() {
  if (Tab* tab = active_tab()) tab->external_notice_pending = false;
  update_ui();
}

void EditorWindow::show_search(const bool replace) {
  const Tab* tab = active_tab();
  const bool hex = tab && tab->view_kind == ViewKind::Hex;
  const bool allow_replace = replace && tab && editable(*tab);
  ShowWindow(search_panel_, SW_SHOW); ShowWindow(find_text_, SW_SHOW); ShowWindow(find_button_, SW_SHOW);
  ShowWindow(regex_check_, hex ? SW_HIDE : SW_SHOW);
  ShowWindow(case_check_, SW_SHOW);
  ShowWindow(all_tabs_check_, hex ? SW_HIDE : SW_SHOW);
  ShowWindow(whole_word_check_, hex ? SW_HIDE : SW_SHOW);
  ShowWindow(wrap_check_, SW_SHOW);
  ShowWindow(selection_only_check_, hex ? SW_HIDE : SW_SHOW);
  ShowWindow(replace_text_, allow_replace ? SW_SHOW : SW_HIDE);
  ShowWindow(replace_button_, allow_replace ? SW_SHOW : SW_HIDE);
  ShowWindow(replace_all_button_, allow_replace ? SW_SHOW : SW_HIDE);
  update_layout(); SetFocus(find_text_); Edit_SetSel(find_text_, 0, -1);
}

void EditorWindow::find_next() {
  Tab* tab = active_tab();
  if (!tab) return;
  SearchOptions options;
  options.regular_expression = Button_GetCheck(regex_check_) == BST_CHECKED;
  options.match_case = Button_GetCheck(case_check_) == BST_CHECKED;
  options.whole_word = Button_GetCheck(whole_word_check_) == BST_CHECKED;
  const std::string pattern = control_text_utf8(find_text_);
  if (pattern.empty()) return;
  const bool wrap = Button_GetCheck(wrap_check_) == BST_CHECKED;
  switch (tab->view_kind) {
    case ViewKind::LargeText:
      LargeFileView::find_next(tab->view, pattern, options, wrap);
      return;
    case ViewKind::Hex:
      options.regular_expression = false;
      options.whole_word = false;
      HexViewWindow::find_next(tab->view, pattern, options, wrap);
      return;
    case ViewKind::Text:
      break;
  }
  const bool selection_only = Button_GetCheck(selection_only_check_) == BST_CHECKED;
  const bool all_tabs = !selection_only && Button_GetCheck(all_tabs_check_) == BST_CHECKED;
  const int first = active_index();
  std::vector<int> order{first};
  if (all_tabs) {
    for (int index = first + 1; index < static_cast<int>(documents_.size()); ++index) order.push_back(index);
    if (wrap) for (int index = 0; index < first; ++index) order.push_back(index);
  }

  std::vector<SearchTabResult> snapshots;
  for (const int index : order) {
    Tab& item = documents_[index];
    if (!editable(item)) continue;
    SearchTabResult snapshot;
    snapshot.index = index;
    snapshot.original = editor_text(item.view);
    snapshot.subject = snapshot.original;
    snapshot.caret = index == first
        ? static_cast<std::size_t>(sci(item.view, SCI_GETCURRENTPOS))
        : 0;
    if (selection_only && index == first) {
      const auto begin = static_cast<std::size_t>(sci(item.view, SCI_GETSELECTIONSTART));
      const auto end = static_cast<std::size_t>(sci(item.view, SCI_GETSELECTIONEND));
      if (end <= begin) { MessageBeep(MB_ICONINFORMATION); return; }
      snapshot.base = begin;
      snapshot.subject = snapshot.original.substr(begin, end - begin);
      snapshot.caret = std::clamp(snapshot.caret, begin, end) - begin;
    }
    snapshots.push_back(std::move(snapshot));
  }
  if (snapshots.empty()) return;

  search_thread_.request_stop();
  const std::uint64_t generation = ++search_generation_;
  const HWND destination = window_;
  SendMessageW(status_, SB_SETTEXTW, 0,
               pointer_param(tr(L"Поиск…", L"Searching…")));
  search_thread_ = std::jthread(
      [destination, generation, wrap, options, pattern,
       snapshots = std::move(snapshots)](const std::stop_token stop) mutable {
        auto completed = std::make_unique<SearchJobResult>();
        completed->kind = SearchJobKind::Find;
        completed->generation = generation;
        completed->wrap = wrap;
        completed->tabs = std::move(snapshots);
        for (auto& item : completed->tabs) {
          item.found = search_all(item.subject, pattern, options, stop);
          if (stop.stop_requested()) return;
        }
        SearchJobResult* raw = completed.release();
        if (!PostMessageW(destination, EditorWindow::kSearchResultMessage, 0,
                          reinterpret_cast<LPARAM>(raw))) delete raw;
      });
}

void EditorWindow::replace_one() {
  Tab* tab = active_tab();
  if (!tab || !editable(*tab)) return;
  SearchOptions options;
  options.regular_expression = Button_GetCheck(regex_check_) == BST_CHECKED;
  options.match_case = Button_GetCheck(case_check_) == BST_CHECKED;
  options.whole_word = Button_GetCheck(whole_word_check_) == BST_CHECKED;
  const std::string pattern = control_text_utf8(find_text_);
  const std::string replacement = control_text_utf8(replace_text_);
  const auto begin = static_cast<std::size_t>(sci(tab->view, SCI_GETSELECTIONSTART));
  const auto end = static_cast<std::size_t>(sci(tab->view, SCI_GETSELECTIONEND));
  if (!pattern.empty() && end > begin) {
    const std::string selected = editor_text(tab->view).substr(begin, end - begin);
    const SearchResult matches = search_all(selected, pattern, options);
    if (!matches.ok) {
      MessageBoxW(window_, utf8_to_wide(matches.error).c_str(), LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
      return;
    }
    if (matches.matches.size() == 1 && matches.matches.front().start == 0 &&
        matches.matches.front().length == selected.size()) {
      const ReplaceResult replaced = replace_all(selected, pattern, replacement, options);
      if (!replaced.ok) {
        MessageBoxW(window_, utf8_to_wide(replaced.error).c_str(), LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
        return;
      }
      sci(tab->view, SCI_BEGINUNDOACTION);
      sci(tab->view, SCI_SETTARGETSTART, begin);
      sci(tab->view, SCI_SETTARGETEND, end);
      sci(tab->view, SCI_REPLACETARGET, replaced.text.size(), pointer_param(replaced.text.data()));
      sci(tab->view, SCI_ENDUNDOACTION);
      find_next();
      return;
    }
  }
  find_next();
}

void EditorWindow::replace_all_open_tabs() {
  SearchOptions options;
  options.regular_expression = Button_GetCheck(regex_check_) == BST_CHECKED;
  options.match_case = Button_GetCheck(case_check_) == BST_CHECKED;
  options.whole_word = Button_GetCheck(whole_word_check_) == BST_CHECKED;
  const std::string pattern = control_text_utf8(find_text_);
  const std::string replacement = control_text_utf8(replace_text_);
  if (pattern.empty()) return;
  const bool selection_only = Button_GetCheck(selection_only_check_) == BST_CHECKED;
  const bool all_tabs = !selection_only && Button_GetCheck(all_tabs_check_) == BST_CHECKED;
  const int active = active_index();
  std::vector<SearchTabResult> snapshots;
  for (int index = 0; index < static_cast<int>(documents_.size()); ++index) {
    if (!all_tabs && index != active) continue;
    Tab& tab = documents_[index];
    if (!editable(tab)) continue;
    SearchTabResult snapshot;
    snapshot.index = index;
    snapshot.original = editor_text(tab.view);
    snapshot.subject = snapshot.original;
    if (selection_only && index == active) {
      const auto begin = static_cast<std::size_t>(sci(tab.view, SCI_GETSELECTIONSTART));
      const auto end = static_cast<std::size_t>(sci(tab.view, SCI_GETSELECTIONEND));
      if (end <= begin) { MessageBeep(MB_ICONINFORMATION); return; }
      snapshot.base = begin;
      snapshot.subject = snapshot.original.substr(begin, end - begin);
    }
    snapshots.push_back(std::move(snapshot));
  }
  if (snapshots.empty()) return;

  search_thread_.request_stop();
  const std::uint64_t generation = ++search_generation_;
  const HWND destination = window_;
  SendMessageW(status_, SB_SETTEXTW, 0,
               pointer_param(tr(L"Замена…", L"Replacing…")));
  search_thread_ = std::jthread(
      [destination, generation, options, pattern, replacement,
       snapshots = std::move(snapshots)](const std::stop_token stop) mutable {
        auto completed = std::make_unique<SearchJobResult>();
        completed->kind = SearchJobKind::ReplaceAll;
        completed->generation = generation;
        completed->tabs = std::move(snapshots);
        for (auto& item : completed->tabs) {
          item.replaced = replace_all(item.subject, pattern, replacement, options, stop);
          if (stop.stop_requested()) return;
        }
        SearchJobResult* raw = completed.release();
        if (!PostMessageW(destination, EditorWindow::kSearchResultMessage, 0,
                          reinterpret_cast<LPARAM>(raw))) delete raw;
      });
}

void EditorWindow::handle_search_result(void* raw_result) {
  std::unique_ptr<SearchJobResult> result(static_cast<SearchJobResult*>(raw_result));
  if (!result || result->generation != search_generation_) return;

  for (const auto& item : result->tabs) {
    const std::string& error = result->kind == SearchJobKind::Find
        ? item.found.error : item.replaced.error;
    const bool ok = result->kind == SearchJobKind::Find
        ? item.found.ok : item.replaced.ok;
    if (!ok) {
      if (error != "Search cancelled")
        MessageBoxW(window_, utf8_to_wide(error).c_str(), LISTOPAD_PRODUCT_NAME, MB_ICONERROR);
      update_ui();
      return;
    }
    if (item.index < 0 || item.index >= static_cast<int>(documents_.size()) ||
        editor_text(documents_[item.index].view) != item.original) {
      update_ui();
      return;
    }
  }

  if (result->kind == SearchJobKind::Find) {
    const SearchTabResult* selected_tab = nullptr;
    const SearchMatch* selected_match = nullptr;
    for (std::size_t index = 0; index < result->tabs.size() && !selected_match; ++index) {
      const auto& item = result->tabs[index];
      const std::size_t from = index == 0 ? item.caret : 0;
      const auto found = std::find_if(item.found.matches.begin(), item.found.matches.end(),
                                      [from](const SearchMatch& match) { return match.start >= from; });
      if (found != item.found.matches.end()) {
        selected_tab = &item;
        selected_match = &*found;
      }
    }
    if (!selected_match && result->wrap && !result->tabs.empty()) {
      const auto& first = result->tabs.front();
      const auto found = std::find_if(first.found.matches.begin(), first.found.matches.end(),
                                     [&](const SearchMatch& match) { return match.start < first.caret; });
      if (found != first.found.matches.end()) {
        selected_tab = &first;
        selected_match = &*found;
      }
    }
    if (!selected_tab || !selected_match) {
      MessageBeep(MB_ICONINFORMATION);
      update_ui();
      return;
    }
    activate_tab(selected_tab->index);
    Tab& tab = documents_[selected_tab->index];
    const std::size_t start = selected_tab->base + selected_match->start;
    sci(tab.view, SCI_SETSEL, start + selected_match->length, start);
    SetFocus(tab.view);
    return;
  }

  std::size_t total = 0;
  for (const auto& item : result->tabs) {
    if (!item.replaced.replacements) continue;
    Tab& tab = documents_[item.index];
    sci(tab.view, SCI_BEGINUNDOACTION);
    sci(tab.view, SCI_SETTARGETSTART, item.base);
    sci(tab.view, SCI_SETTARGETEND, item.base + item.subject.size());
    sci(tab.view, SCI_REPLACETARGET, item.replaced.text.size(),
        pointer_param(item.replaced.text.data()));
    sci(tab.view, SCI_ENDUNDOACTION);
    total += item.replaced.replacements;
  }
  std::wstring message = tr(L"Замен: ", L"Replacements: ");
  message += std::to_wstring(total);
  SendMessageW(status_, SB_SETTEXTW, 0, pointer_param(message.c_str()));
  update_ui();
}

void EditorWindow::format_active() {
  Tab* tab = active_tab(); if (!tab || !editable(*tab)) return;
  FormatKind kind;
  if (tab->document.language == "json") kind = FormatKind::Json;
  else if (tab->document.language == "xml") kind = FormatKind::Xml;
  else if (tab->document.language == "html") kind = FormatKind::Html;
  else { MessageBeep(MB_ICONINFORMATION); return; }
  const auto start = static_cast<std::size_t>(sci(tab->view, SCI_GETSELECTIONSTART));
  const auto end = static_cast<std::size_t>(sci(tab->view, SCI_GETSELECTIONEND));
  const std::string full = editor_text(tab->view);
  const std::string_view input = end > start ? std::string_view(full).substr(start, end - start) : std::string_view(full);
  FormatOptions options;
  options.indent = settings_.indent_with_tabs ? "\t" : std::string(settings_.indent_size, ' ');
  options.eol = std::string(eol_text(tab->document.eol));
  const FormatResult formatted = format_text(kind, input, options);
  if (!formatted.ok) {
    std::wstring message = utf8_to_wide(formatted.error);
    if (formatted.line) {
      message += L"\n";
      message += tr(L"Строка ", L"Line ");
      message += std::to_wstring(formatted.line);
      message += tr(L", столбец ", L", column ");
      message += std::to_wstring(formatted.column);
    }
    MessageBoxW(window_, message.c_str(), LISTOPAD_PRODUCT_NAME, MB_ICONERROR); return;
  }
  const std::size_t target_start = end > start ? start : 0;
  const std::size_t target_end = end > start ? end : full.size();
  sci(tab->view, SCI_BEGINUNDOACTION); sci(tab->view, SCI_SETTARGETSTART, target_start);
  sci(tab->view, SCI_SETTARGETEND, target_end);
  sci(tab->view, SCI_REPLACETARGET, formatted.text.size(), pointer_param(formatted.text.data())); sci(tab->view, SCI_ENDUNDOACTION);
}

bool EditorWindow::try_emmet(HWND editor, const bool backwards) {
  Tab* tab = active_tab(); if (!tab || tab->view != editor || !editable(*tab)) return false;
  if (!tab->snippet_fields.empty()) {
    if (backwards && tab->snippet_index > 0) --tab->snippet_index;
    else if (!backwards && tab->snippet_index + 1 < tab->snippet_fields.size()) ++tab->snippet_index;
    else { tab->snippet_fields.clear(); return false; }
    const auto& field = tab->snippet_fields[tab->snippet_index];
    sci(editor, SCI_SETSEL, field.start + field.length, field.start); return true;
  }
  if (backwards) return false;
  const LanguageInfo* language = language_by_id(tab->document.language);
  if (!language || (!language->emmet_markup && !language->emmet_stylesheet)) return false;
  const std::string text = editor_text(editor);
  const std::size_t caret = static_cast<std::size_t>(sci(editor, SCI_GETCURRENTPOS));
  std::size_t start = caret;
  while (start > 0 && caret - start < 512) {
    const char ch = text[start - 1];
    if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ' || ch == '<' || ch == '"' || ch == '\'') break;
    --start;
  }
  if (start == caret) return false;
  const std::string syntax = language->emmet_stylesheet ? (tab->document.language == "scss" ? "scss" : "css")
                                                        : (tab->document.language == "xml" ? "xml" : "html");
  EmmetExpansion expansion = emmet_.expand(std::string_view(text).substr(start, caret - start), syntax);
  if (!expansion.ok || expansion.text.empty()) return false;
  sci(editor, SCI_BEGINUNDOACTION); sci(editor, SCI_SETTARGETSTART, start); sci(editor, SCI_SETTARGETEND, caret);
  sci(editor, SCI_REPLACETARGET, expansion.text.size(), pointer_param(expansion.text.data())); sci(editor, SCI_ENDUNDOACTION);
  tab->snippet_fields = std::move(expansion.fields); tab->snippet_index = 0;
  for (auto& field : tab->snippet_fields) field.start += start;
  if (!tab->snippet_fields.empty()) {
    const auto& field = tab->snippet_fields.front(); sci(editor, SCI_SETSEL, field.start + field.length, field.start);
  } else sci(editor, SCI_GOTOPOS, start + expansion.text.size());
  return true;
}

LRESULT CALLBACK EditorWindow::editor_subclass(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam, UINT_PTR, DWORD_PTR data) {
  auto* self = reinterpret_cast<EditorWindow*>(data);
  if (message == WM_KEYDOWN && self) {
    if (wparam == VK_TAB && self->try_emmet(window, (GetKeyState(VK_SHIFT) & 0x8000) != 0)) return 0;
    if (wparam == VK_ESCAPE) if (Tab* tab = self->active_tab()) tab->snippet_fields.clear();
  } else if (message == WM_NCDESTROY) RemoveWindowSubclass(window, editor_subclass, 1);
  return DefSubclassProc(window, message, wparam, lparam);
}

void EditorWindow::on_notify(const NMHDR& notification) {
  if (notification.hwndFrom == tabs_ && notification.code == TCN_SELCHANGE) { activate_tab(TabCtrl_GetCurSel(tabs_)); return; }
  if (notification.code == SCN_SAVEPOINTLEFT ||
      notification.code == SCN_SAVEPOINTREACHED ||
      notification.code == SCN_UPDATEUI ||
      notification.code == SCN_MODIFIED) {
    bool handled = false;
    for (auto& tab : documents_) if (tab.view == notification.hwndFrom) {
      if (notification.code == SCN_SAVEPOINTLEFT) tab.document.dirty = true;
      if (notification.code == SCN_SAVEPOINTREACHED) tab.document.dirty = false;
      if (tab.map &&
          (notification.code == SCN_UPDATEUI ||
           notification.code == SCN_MODIFIED)) {
        DocumentMap::sync(tab.map, tab.view);
      }
      handled = true;
      break;
    }
    if (handled && notification.code != SCN_MODIFIED) update_ui();
  }
}

void EditorWindow::on_command(const int command, int, HWND) {
  Tab* tab = active_tab();
  if (command >= static_cast<int>(kLanguageFirst) &&
      command < static_cast<int>(kLanguageFirst + language_menu_ids_.size())) {
    if (tab && editable(*tab))
      apply_language(*tab, language_menu_ids_[command - kLanguageFirst]);
    return;
  }
  switch (command) {
    case IDM_FILE_NEW: add_empty_tab(); break;
    case IDM_FILE_OPEN: { const auto path = choose_open_file(); if (!path.empty()) open_file(path); break; }
    case IDM_FILE_SAVE: if (tab) save_tab(*tab); break;
    case IDM_FILE_SAVE_AS: if (tab) save_tab(*tab, true); break;
    case IDM_FILE_CLOSE: close_tab(active_index()); break;
    case IDM_FILE_EXIT: PostMessageW(window_, WM_CLOSE, 0, 0); break;
    case IDM_ENCODING_UTF8: reopen_active({EncodingKind::Utf8, CP_UTF8, false}); break;
    case IDM_ENCODING_UTF8_BOM: reopen_active({EncodingKind::Utf8, CP_UTF8, true}); break;
    case IDM_ENCODING_UTF16LE: reopen_active({EncodingKind::Utf16Le, 1200, true}); break;
    case IDM_ENCODING_UTF16BE: reopen_active({EncodingKind::Utf16Be, 1201, true}); break;
    case IDM_ENCODING_CP1251: reopen_active({EncodingKind::WindowsCodePage, 1251, false}); break;
    case IDM_ENCODING_CP866: reopen_active({EncodingKind::WindowsCodePage, 866, false}); break;
    case IDM_ENCODING_CP1252: reopen_active({EncodingKind::WindowsCodePage, 1252, false}); break;
    case IDM_EDIT_UNDO: if (tab && editable(*tab)) sci(tab->view, SCI_UNDO); break;
    case IDM_EDIT_REDO: if (tab && editable(*tab)) sci(tab->view, SCI_REDO); break;
    case IDM_EDIT_CUT: if (tab && editable(*tab)) sci(tab->view, SCI_CUT); break;
    case IDM_EDIT_COPY: if (tab && editable(*tab)) sci(tab->view, SCI_COPY); break;
    case IDM_EDIT_PASTE: if (tab && editable(*tab)) sci(tab->view, SCI_PASTE); break;
    case IDM_EDIT_SELECT_ALL: if (tab && editable(*tab)) sci(tab->view, SCI_SELECTALL); break;
    case IDM_SEARCH_FIND: show_search(false); break;
    case IDM_SEARCH_REPLACE: show_search(true); break;
    case IDM_SEARCH_NEXT: case IDC_FIND_NEXT: find_next(); break;
    case IDC_REPLACE_ALL: replace_all_open_tabs(); break;
    case IDC_REPLACE_ONE: replace_one(); break;
    case IDC_BANNER_RELOAD: reload_active(); break;
    case IDC_BANNER_KEEP: keep_external_active(); break;
    case IDM_VIEW_DOCUMENT_MAP:
      settings_.show_document_map = !settings_.show_document_map;
      save_settings(settings_);
      refresh_view_menu_state();
      update_layout();
      break;
    case IDM_VIEW_HEX:
      if (!tab || !tab->document.has_path() || tab->document.dirty ||
          tab->document.external_diverged) {
        MessageBeep(MB_ICONINFORMATION);
        MessageBoxW(
            window_,
            tr(L"Переключение Hex/ASCII доступно только для сохранённого "
               L"файла без локальных или внешних изменений.",
               L"Hex/ASCII switching is available only for a saved file "
                L"without local or external changes."),
            LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION);
      } else {
        switch_tab_view(*tab, tab->view_kind == ViewKind::Hex
                                  ? ViewKind::Text
                                  : ViewKind::Hex);
      }
      break;
    case IDM_TOOLS_FORMAT: format_active(); break;
    case IDM_TOOLS_REGISTER:
      MessageBoxW(window_, register_classic_context_menu(settings_.ui_language)
                               ? tr(L"Пункт контекстного меню добавлен.", L"Context menu command registered.")
                               : tr(L"Не удалось добавить пункт контекстного меню.", L"Unable to register context menu command."),
                  LISTOPAD_PRODUCT_NAME, MB_OK); break;
    case IDM_TOOLS_UNREGISTER:
      MessageBoxW(window_, unregister_classic_context_menu()
                               ? tr(L"Пункт контекстного меню удалён.", L"Context menu command removed.")
                               : tr(L"Не удалось удалить пункт контекстного меню.", L"Unable to unregister context menu command."),
                  LISTOPAD_PRODUCT_NAME, MB_OK); break;
    case IDM_HELP_ABOUT:
      MessageBoxW(window_, L"Listopad++ " LISTOPAD_VERSION_WSTRING L"\n\nFast offline Windows text editor\nScintilla · Lexilla · PCRE2 · Emmet",
                  LISTOPAD_PRODUCT_NAME, MB_OK | MB_ICONINFORMATION); break;
    default: break;
  }
}

void EditorWindow::update_ui() {
  Tab* tab = active_tab(); if (!tab) return;
  for (int index = 0; index < static_cast<int>(documents_.size()); ++index) {
    std::wstring title = documents_[index].document.title;
    if (documents_[index].document.dirty) title += L" *";
    if (documents_[index].document.external_diverged) title += L" !";
    title = fit_tab_title(std::move(title));
    TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = title.data(); TabCtrl_SetItem(tabs_, index, &item);
  }
  const bool show_external_notice = tab->external_notice_pending;
  if (show_external_notice) {
    SetWindowTextW(banner_, tr(L"Файл изменён другой программой",
                               L"The file was changed by another program"));
  }
  ShowWindow(banner_, show_external_notice ? SW_SHOW : SW_HIDE);
  ShowWindow(reload_button_, show_external_notice ? SW_SHOW : SW_HIDE);
  ShowWindow(keep_button_, show_external_notice ? SW_SHOW : SW_HIDE);
  std::array<int, 5> parts{180, 330, 460, 610, -1}; SendMessageW(status_, SB_SETPARTS, parts.size(), pointer_param(parts.data()));
  const std::wstring encoding = utf8_to_wide(tab->document.encoding.name());
  const std::wstring eol = eol_name(tab->document.eol);
  const LanguageInfo* language = language_by_id(tab->document.language);
  const std::wstring language_name = language ? utf8_to_wide(language->display_name)
                                               : utf8_to_wide(tab->document.language);
  switch (tab->view_kind) {
    case ViewKind::LargeText:
      SendMessageW(status_, SB_SETTEXTW, 0,
                   pointer_param(tr(L"Большой файл · только чтение",
                                    L"Large file · read-only")));
      SendMessageW(status_, SB_SETTEXTW, 1, pointer_param(encoding.c_str()));
      SendMessageW(status_, SB_SETTEXTW, 2, pointer_param(eol.c_str()));
      SendMessageW(status_, SB_SETTEXTW, 3, pointer_param(language_name.c_str()));
      break;
    case ViewKind::Hex: {
      SendMessageW(status_, SB_SETTEXTW, 0,
                   pointer_param(tr(L"Hex · только чтение",
                                    L"Hex · read-only")));
      const std::wstring size =
          std::to_wstring(tab->document.fingerprint.size) +
          tr(L" байт", L" bytes");
      SendMessageW(status_, SB_SETTEXTW, 1, pointer_param(size.c_str()));
      SendMessageW(status_, SB_SETTEXTW, 2, pointer_param(L"—"));
      SendMessageW(status_, SB_SETTEXTW, 3, pointer_param(L"—"));
      break;
    }
    case ViewKind::Text: {
      const auto position =
          static_cast<sptr_t>(sci(tab->view, SCI_GETCURRENTPOS));
      const auto line =
          static_cast<sptr_t>(sci(tab->view, SCI_LINEFROMPOSITION, position));
      const auto column =
          static_cast<sptr_t>(sci(tab->view, SCI_GETCOLUMN, position));
      const std::wstring location =
          tr(L"Стр ", L"Ln ") + std::to_wstring(line + 1) +
          tr(L", стлб ", L", Col ") + std::to_wstring(column + 1);
      SendMessageW(status_, SB_SETTEXTW, 0,
                   pointer_param(location.c_str()));
      SendMessageW(status_, SB_SETTEXTW, 1, pointer_param(encoding.c_str()));
      SendMessageW(status_, SB_SETTEXTW, 2, pointer_param(eol.c_str()));
      SendMessageW(status_, SB_SETTEXTW, 3,
                   pointer_param(language_name.c_str()));
      break;
    }
  }
  SendMessageW(status_, SB_SETTEXTW, 4, pointer_param(tab->document.external_diverged ? tr(L"Внешние изменения", L"External changes") : L""));
  std::wstring window_title = tab->document.title + L" — " LISTOPAD_PRODUCT_NAME;
  SetWindowTextW(window_, window_title.c_str());
  refresh_view_menu_state();
  update_layout();
  if (show_external_notice) {
    RedrawWindow(banner_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    RedrawWindow(reload_button_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    RedrawWindow(keep_button_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
  }
}

std::filesystem::path EditorWindow::choose_open_file() {
  std::wstring buffer(32768, L'\0');
  OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner = window_; dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrFilter = tr(L"Все файлы\0*.*\0Текстовые файлы\0*.txt;*.log;*.json;*.xml;*.html\0\0",
                          L"All files\0*.*\0Text files\0*.txt;*.log;*.json;*.xml;*.html\0\0");
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  return GetOpenFileNameW(&dialog) ? std::filesystem::path(buffer.data()) : std::filesystem::path{};
}

std::filesystem::path EditorWindow::choose_save_file(const Tab& tab) {
  std::wstring filters;
  std::vector<const LanguageInfo*> filter_languages;
  const auto append_filter = [&filters](const std::wstring_view label,
                                        const std::wstring_view pattern) {
    filters.append(label);
    filters.push_back(L'\0');
    filters.append(pattern);
    filters.push_back(L'\0');
  };
  for (const LanguageInfo& language : languages()) {
    std::wstring pattern;
    for (const std::string& extension : language.extensions) {
      if (!pattern.empty()) pattern.push_back(L';');
      pattern.push_back(L'*');
      pattern += utf8_to_wide(extension);
    }
    std::wstring label = utf8_to_wide(language.display_name);
    label += L" (";
    label += pattern;
    label.push_back(L')');
    append_filter(label, pattern);
    filter_languages.push_back(&language);
  }
  append_filter(tr(L"Все файлы (*.*)", L"All files (*.*)"), L"*.*");
  filter_languages.push_back(nullptr);
  filters.push_back(L'\0');

  const LanguageInfo* initial_language = nullptr;
  if (tab.document.has_path()) {
    initial_language = language_by_extension(
        wide_to_utf8(tab.document.path.extension().wstring()));
  } else {
    initial_language = language_by_id(tab.document.language);
  }
  DWORD initial_filter = static_cast<DWORD>(filter_languages.size());
  if (initial_language) {
    const auto found =
        std::find(filter_languages.begin(), filter_languages.end(),
                  initial_language);
    if (found != filter_languages.end()) {
      initial_filter =
          static_cast<DWORD>(std::distance(filter_languages.begin(), found) + 1);
    }
  }

  std::wstring buffer =
      tab.document.has_path() ? tab.document.path.wstring()
                              : tab.document.title;
  buffer.resize(32768, L'\0');
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.hwndOwner = window_;
  dialog.lpstrFile = buffer.data();
  dialog.nMaxFile = static_cast<DWORD>(buffer.size());
  dialog.lpstrFilter = filters.c_str();
  dialog.nFilterIndex = initial_filter;
  dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
  if (!GetSaveFileNameW(&dialog)) return {};

  const std::filesystem::path selected(buffer.data());
  std::filesystem::path target = selected;
  const std::size_t filter_index =
      dialog.nFilterIndex > 0
          ? static_cast<std::size_t>(dialog.nFilterIndex - 1)
          : filter_languages.size();
  if (filter_index < filter_languages.size() &&
      filter_languages[filter_index]) {
    target = append_default_extension(
        target, filter_languages[filter_index]->id);
  }
  std::error_code exists_error;
  if (target != selected &&
      std::filesystem::exists(target, exists_error)) {
    const std::wstring message =
        tr(L"Файл уже существует:\n", L"The file already exists:\n") +
        target.wstring() +
        tr(L"\n\nПерезаписать его?", L"\n\nOverwrite it?");
    if (MessageBoxW(window_, message.c_str(), LISTOPAD_PRODUCT_NAME,
                    MB_YESNO | MB_ICONWARNING) != IDYES) {
      return {};
    }
  }
  return target;
}

}  // namespace listopad::app
