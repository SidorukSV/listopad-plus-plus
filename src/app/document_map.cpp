#include "document_map.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <Scintilla.h>

#include <algorithm>
#include <string>

namespace listopad::app {
namespace {

constexpr UINT_PTR kSubclassId = 1;

struct State {
  HWND editor{nullptr};
  bool dark{false};
  bool dragging{false};
};

LRESULT sci(const HWND window, const UINT message, const WPARAM wparam = 0,
            const LPARAM lparam = 0) {
  return SendMessageW(window, message, wparam, lparam);
}

void navigate_to_point(const HWND map, State& state, const int x, const int y) {
  if (!state.editor) return;
  sptr_t position = static_cast<sptr_t>(
      sci(map, SCI_POSITIONFROMPOINTCLOSE, static_cast<WPARAM>(x),
          static_cast<LPARAM>(y)));
  if (position < 0) {
    RECT client{};
    GetClientRect(map, &client);
    position = y < client.top ? 0 : static_cast<sptr_t>(sci(map, SCI_GETLENGTH));
  }
  const auto document_line =
      static_cast<sptr_t>(sci(map, SCI_LINEFROMPOSITION, position));
  const auto visible_line =
      static_cast<sptr_t>(sci(state.editor, SCI_VISIBLEFROMDOCLINE, document_line));
  const auto page =
      std::max<sptr_t>(1, static_cast<sptr_t>(sci(state.editor, SCI_LINESONSCREEN)));
  sci(state.editor, SCI_SETFIRSTVISIBLELINE,
      static_cast<WPARAM>(std::max<sptr_t>(0, visible_line - page / 2)));
  SetFocus(state.editor);
}

void paint_overlay(const HWND map, const State& state) {
  if (!state.editor) return;
  RECT client{};
  GetClientRect(map, &client);
  if (client.right <= client.left || client.bottom <= client.top) return;

  const auto editor_first_visible =
      static_cast<sptr_t>(sci(state.editor, SCI_GETFIRSTVISIBLELINE));
  const auto editor_visible_count =
      std::max<sptr_t>(1, static_cast<sptr_t>(sci(state.editor, SCI_LINESONSCREEN)));
  const auto editor_first_document =
      static_cast<sptr_t>(sci(state.editor, SCI_DOCLINEFROMVISIBLE,
                              static_cast<WPARAM>(editor_first_visible)));
  const auto editor_last_document =
      static_cast<sptr_t>(sci(
          state.editor, SCI_DOCLINEFROMVISIBLE,
          static_cast<WPARAM>(editor_first_visible + editor_visible_count - 1)));
  const auto map_first_visible =
      static_cast<sptr_t>(sci(map, SCI_GETFIRSTVISIBLELINE));
  const auto map_first_document =
      static_cast<sptr_t>(sci(map, SCI_DOCLINEFROMVISIBLE,
                              static_cast<WPARAM>(map_first_visible)));
  const int line_height =
      std::max(1, static_cast<int>(sci(map, SCI_TEXTHEIGHT, 0)));

  int top = static_cast<int>((editor_first_document - map_first_document) *
                             line_height);
  int bottom = static_cast<int>(
      (std::max(editor_first_document, editor_last_document) -
       map_first_document + 1) *
      line_height);
  top = std::clamp(
      top, static_cast<int>(client.top),
      static_cast<int>(std::max(client.top, client.bottom - 1)));
  bottom = std::clamp(bottom, top + 1, static_cast<int>(client.bottom));
  RECT viewport{client.left, top, client.right, bottom};

  HDC dc = GetDC(map);
  if (!dc) return;
  HDC source = CreateCompatibleDC(dc);
  HBITMAP pixel = source ? CreateCompatibleBitmap(dc, 1, 1) : nullptr;
  HGDIOBJ old_bitmap = nullptr;
  if (source && pixel) {
    old_bitmap = SelectObject(source, pixel);
    SetDCBrushColor(source, state.dark ? RGB(62, 95, 135) : RGB(190, 215, 245));
    RECT one{0, 0, 1, 1};
    FillRect(source, &one, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 80, 0};
    AlphaBlend(dc, viewport.left, viewport.top,
               viewport.right - viewport.left, viewport.bottom - viewport.top,
               source, 0, 0, 1, 1, blend);
  }
  HBRUSH frame = CreateSolidBrush(
      state.dark ? RGB(125, 170, 225) : RGB(62, 95, 135));
  if (frame) {
    FrameRect(dc, &viewport, frame);
    DeleteObject(frame);
  }
  if (old_bitmap) SelectObject(source, old_bitmap);
  if (pixel) DeleteObject(pixel);
  if (source) DeleteDC(source);
  ReleaseDC(map, dc);
}

LRESULT CALLBACK subclass_proc(const HWND map, const UINT message,
                               const WPARAM wparam, const LPARAM lparam,
                               const UINT_PTR id, const DWORD_PTR data) {
  auto* state = reinterpret_cast<State*>(data);
  switch (message) {
    case WM_NCDESTROY: {
      RemoveWindowSubclass(map, subclass_proc, id);
      delete state;
      return DefSubclassProc(map, message, wparam, lparam);
    }
    case WM_SETFOCUS:
      if (state && state->editor) SetFocus(state->editor);
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_GETDLGCODE:
      return 0;
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_DROPFILES:
    case WM_CONTEXTMENU:
      return 0;
    case WM_LBUTTONDOWN:
      if (state) {
        state->dragging = true;
        SetCapture(map);
        navigate_to_point(map, *state, GET_X_LPARAM(lparam),
                          GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_MOUSEMOVE:
      if (state && state->dragging && (wparam & MK_LBUTTON)) {
        navigate_to_point(map, *state, GET_X_LPARAM(lparam),
                          GET_Y_LPARAM(lparam));
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (state && state->dragging) {
        state->dragging = false;
        if (GetCapture() == map) ReleaseCapture();
        navigate_to_point(map, *state, GET_X_LPARAM(lparam),
                          GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (state) state->dragging = false;
      break;
    case WM_MOUSEWHEEL:
      if (state && state->editor)
        SendMessageW(state->editor, message, wparam, lparam);
      return 0;
    case WM_PAINT: {
      const LRESULT result = DefSubclassProc(map, message, wparam, lparam);
      if (state) paint_overlay(map, *state);
      return result;
    }
    default:
      break;
  }
  return DefSubclassProc(map, message, wparam, lparam);
}

State* map_state(const HWND map) {
  DWORD_PTR data = 0;
  if (!GetWindowSubclass(map, subclass_proc, kSubclassId, &data)) return nullptr;
  return reinterpret_cast<State*>(data);
}

}  // namespace

HWND DocumentMap::create(const HWND parent, const int control_id,
                         const HINSTANCE instance) {
  HWND map = CreateWindowExW(
      0, L"Scintilla", L"", WS_CHILD | WS_CLIPSIBLINGS,
      0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance,
      nullptr);
  if (!map) return nullptr;
  auto* state = new State;
  if (!SetWindowSubclass(map, subclass_proc, kSubclassId,
                         reinterpret_cast<DWORD_PTR>(state))) {
    delete state;
    DestroyWindow(map);
    return nullptr;
  }
  return map;
}

void DocumentMap::attach(const HWND map, const HWND editor) {
  if (!map || !editor) return;
  if (State* state = map_state(map)) state->editor = editor;
  sci(map, SCI_SETDOCPOINTER, 0, sci(editor, SCI_GETDOCPOINTER));
  for (int margin = 0; margin < 5; ++margin)
    sci(map, SCI_SETMARGINWIDTHN, margin, 0);
  sci(map, SCI_SETVSCROLLBAR, FALSE);
  sci(map, SCI_SETHSCROLLBAR, FALSE);
  sci(map, SCI_SETWRAPMODE, SC_WRAP_NONE);
  sci(map, SCI_SETCARETSTYLE, CARETSTYLE_INVISIBLE);
  sci(map, SCI_SETCARETLINEVISIBLE, FALSE);
  sci(map, SCI_SETEXTRAASCENT, 0);
  sci(map, SCI_SETEXTRADESCENT, 0);
  sci(map, SCI_SETEDGECOLOUR, 0);
  sci(map, SCI_SETEDGEMODE, EDGE_NONE);
  sync(map, editor);
}

void DocumentMap::restyle(const HWND map, const HWND editor,
                          const std::string_view font_face, const bool dark) {
  if (!map || !editor) return;
  if (State* state = map_state(map)) state->dark = dark;
  SetWindowTheme(map, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
  const std::string face(font_face);
  sci(map, SCI_STYLESETFONT, STYLE_DEFAULT,
      reinterpret_cast<LPARAM>(face.c_str()));
  sci(map, SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT,
      2 * SC_FONT_SIZE_MULTIPLIER);
  sci(map, SCI_STYLECLEARALL);
  for (int style = 0; style < 256; ++style) {
    sci(map, SCI_STYLESETFONT, style, reinterpret_cast<LPARAM>(face.c_str()));
    sci(map, SCI_STYLESETSIZEFRACTIONAL, style,
        2 * SC_FONT_SIZE_MULTIPLIER);
    sci(map, SCI_STYLESETFORE, style, sci(editor, SCI_STYLEGETFORE, style));
    sci(map, SCI_STYLESETBACK, style, sci(editor, SCI_STYLEGETBACK, style));
    sci(map, SCI_STYLESETBOLD, style, sci(editor, SCI_STYLEGETBOLD, style));
    sci(map, SCI_STYLESETITALIC, style, sci(editor, SCI_STYLEGETITALIC, style));
  }
  InvalidateRect(map, nullptr, TRUE);
}

void DocumentMap::sync(const HWND map, const HWND editor) {
  if (!map || !editor) return;
  const auto line_count =
      std::max<sptr_t>(1, static_cast<sptr_t>(sci(editor, SCI_GETLINECOUNT)));
  const auto editor_first_visible =
      static_cast<sptr_t>(sci(editor, SCI_GETFIRSTVISIBLELINE));
  const auto editor_first_document =
      std::clamp<sptr_t>(
          static_cast<sptr_t>(sci(editor, SCI_DOCLINEFROMVISIBLE,
                                  static_cast<WPARAM>(editor_first_visible))),
          0, line_count - 1);
  const auto editor_page =
      std::max<sptr_t>(1, static_cast<sptr_t>(sci(editor, SCI_LINESONSCREEN)));
  const auto map_page =
      std::max<sptr_t>(1, static_cast<sptr_t>(sci(map, SCI_LINESONSCREEN)));
  sptr_t map_first = 0;
  if (line_count > map_page) {
    const sptr_t source_range = std::max<sptr_t>(1, line_count - editor_page);
    const sptr_t map_range = line_count - map_page;
    map_first = static_cast<sptr_t>(
        (static_cast<long double>(editor_first_document) * map_range) /
        source_range);
  }
  sci(map, SCI_SETFIRSTVISIBLELINE,
      static_cast<WPARAM>(std::clamp<sptr_t>(
          map_first, 0, std::max<sptr_t>(0, line_count - map_page))));
  InvalidateRect(map, nullptr, FALSE);
}

}  // namespace listopad::app
