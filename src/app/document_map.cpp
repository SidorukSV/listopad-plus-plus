#include "document_map.h"

#include <Scintilla.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace listopad::app {
namespace {

constexpr wchar_t kClassName[] = L"ListopadPPDocumentMap";
constexpr int kHorizontalPadding = 4;
constexpr int kPreviewColumns = 160;

struct State {
  HWND editor{nullptr};
  bool dark{false};
  bool dragging{false};
  HBITMAP preview{nullptr};
  int preview_width{0};
  int preview_height{0};
  bool preview_dirty{true};
};

LRESULT sci(const HWND window, const UINT message, const WPARAM wparam = 0,
            const LPARAM lparam = 0) {
  return SendMessageW(window, message, wparam, lparam);
}

State* state_for(const HWND window) {
  return reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

void discard_preview(State& state) {
  if (state.preview) DeleteObject(state.preview);
  state.preview = nullptr;
  state.preview_width = 0;
  state.preview_height = 0;
  state.preview_dirty = true;
}

void navigate_to_point(const HWND map, const State& state, const int y) {
  if (!state.editor) return;
  RECT client{};
  GetClientRect(map, &client);
  const int height = std::max(1L, client.bottom - client.top);
  const auto line_count =
      std::max<sptr_t>(1, static_cast<sptr_t>(
                              sci(state.editor, SCI_GETLINECOUNT)));
  const int clamped_y =
      std::clamp(y - static_cast<int>(client.top), 0, height - 1);
  const auto document_line = std::clamp<sptr_t>(
      static_cast<sptr_t>(
          static_cast<long double>(clamped_y) * line_count / height),
      0, line_count - 1);
  const auto visible_line = std::max<sptr_t>(
      0, static_cast<sptr_t>(
             sci(state.editor, SCI_VISIBLEFROMDOCLINE, document_line)));
  const auto page = std::max<sptr_t>(
      1, static_cast<sptr_t>(sci(state.editor, SCI_LINESONSCREEN)));
  sci(state.editor, SCI_SETFIRSTVISIBLELINE,
      static_cast<WPARAM>(std::max<sptr_t>(0, visible_line - page / 2)));
  SetFocus(state.editor);
}

bool line_geometry(const HWND editor, const sptr_t line, int& indentation,
                   int& end_column, COLORREF& colour) {
  const auto line_start =
      static_cast<sptr_t>(sci(editor, SCI_POSITIONFROMLINE, line));
  const auto indentation_position =
      static_cast<sptr_t>(sci(editor, SCI_GETLINEINDENTPOSITION, line));
  const auto line_end =
      static_cast<sptr_t>(sci(editor, SCI_GETLINEENDPOSITION, line));
  if (line_start < 0 || indentation_position < 0 ||
      line_end <= indentation_position) {
    return false;
  }
  indentation = std::max(
      0, static_cast<int>(sci(editor, SCI_GETCOLUMN,
                              static_cast<WPARAM>(indentation_position))));
  end_column = std::max(
      indentation + 1,
      static_cast<int>(
          sci(editor, SCI_GETCOLUMN, static_cast<WPARAM>(line_end))));
  const int style = static_cast<int>(
      sci(editor, SCI_GETSTYLEAT, static_cast<WPARAM>(indentation_position)));
  colour = static_cast<COLORREF>(
      sci(editor, SCI_STYLEGETFORE, static_cast<WPARAM>(style)));
  return true;
}

void draw_preview_line(const HDC dc, const RECT& client, const HWND editor,
                       const sptr_t line, const int y, const int thickness) {
  int indentation = 0;
  int end_column = 0;
  COLORREF colour{};
  if (!line_geometry(editor, line, indentation, end_column, colour)) return;
  const int width = std::max(
      1L, client.right - client.left - kHorizontalPadding * 2);
  const int first_column = std::min(indentation, kPreviewColumns - 1);
  const int last_column =
      std::clamp(end_column, first_column + 1, kPreviewColumns);
  const int x1 = client.left + kHorizontalPadding +
                 MulDiv(first_column, width, kPreviewColumns);
  const int x2 = std::max(
      x1 + 1, static_cast<int>(client.left) + kHorizontalPadding +
                  MulDiv(last_column, width, kPreviewColumns));
  SetDCPenColor(dc, colour);
  for (int offset = 0; offset < thickness; ++offset) {
    MoveToEx(dc, x1, y + offset, nullptr);
    LineTo(dc, x2, y + offset);
  }
}

void draw_document_preview(const HDC dc, const RECT& client,
                           const State& state) {
  if (!state.editor) return;
  const int height = std::max(1L, client.bottom - client.top);
  const auto line_count =
      std::max<sptr_t>(1, static_cast<sptr_t>(
                              sci(state.editor, SCI_GETLINECOUNT)));
  const HGDIOBJ previous_pen = SelectObject(dc, GetStockObject(DC_PEN));

  if (line_count <= height) {
    for (sptr_t line = 0; line < line_count; ++line) {
      const int top = client.top + static_cast<int>(
          static_cast<long double>(line) * height / line_count);
      const int bottom = client.top + static_cast<int>(
          static_cast<long double>(line + 1) * height / line_count);
      const int thickness = std::clamp(bottom - top - 1, 1, 2);
      draw_preview_line(dc, client, state.editor, line,
                        top + std::max(0, (bottom - top - thickness) / 2),
                        thickness);
    }
  } else {
    for (int pixel = 0; pixel < height; ++pixel) {
      const auto line = std::min<sptr_t>(
          line_count - 1,
          static_cast<sptr_t>(
              (static_cast<long double>(pixel) + 0.5L) * line_count /
              height));
      draw_preview_line(dc, client, state.editor, line,
                        client.top + pixel, 1);
    }
  }
  SelectObject(dc, previous_pen);
}

void ensure_preview(const HDC reference, const RECT& client, State& state) {
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  if (!state.preview_dirty && state.preview &&
      state.preview_width == width && state.preview_height == height) {
    return;
  }
  discard_preview(state);
  state.preview = CreateCompatibleBitmap(reference, width, height);
  if (!state.preview) return;
  state.preview_width = width;
  state.preview_height = height;
  HDC preview_dc = CreateCompatibleDC(reference);
  if (!preview_dc) {
    discard_preview(state);
    return;
  }
  const HGDIOBJ previous = SelectObject(preview_dc, state.preview);
  RECT preview_rect{0, 0, width, height};
  SetDCBrushColor(preview_dc,
                  state.dark ? RGB(30, 30, 30)
                             : GetSysColor(COLOR_WINDOW));
  FillRect(preview_dc, &preview_rect,
           static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  draw_document_preview(preview_dc, preview_rect, state);
  SelectObject(preview_dc, previous);
  DeleteDC(preview_dc);
  state.preview_dirty = false;
}

void alpha_fill(const HDC destination, const RECT& rectangle,
                const COLORREF colour, const BYTE alpha) {
  HDC source = CreateCompatibleDC(destination);
  HBITMAP pixel =
      source ? CreateCompatibleBitmap(destination, 1, 1) : nullptr;
  HGDIOBJ old_bitmap = nullptr;
  if (source && pixel) {
    old_bitmap = SelectObject(source, pixel);
    SetDCBrushColor(source, colour);
    RECT one{0, 0, 1, 1};
    FillRect(source, &one,
             static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const BLENDFUNCTION blend{AC_SRC_OVER, 0, alpha, 0};
    AlphaBlend(destination, rectangle.left, rectangle.top,
               rectangle.right - rectangle.left,
               rectangle.bottom - rectangle.top, source, 0, 0, 1, 1,
               blend);
  }
  if (old_bitmap) SelectObject(source, old_bitmap);
  if (pixel) DeleteObject(pixel);
  if (source) DeleteDC(source);
}

void draw_viewport(const HDC dc, const RECT& client, const State& state) {
  if (!state.editor) return;
  const int height = std::max(1L, client.bottom - client.top);
  const auto line_count =
      std::max<sptr_t>(1, static_cast<sptr_t>(
                              sci(state.editor, SCI_GETLINECOUNT)));
  const auto first_visible =
      static_cast<sptr_t>(sci(state.editor, SCI_GETFIRSTVISIBLELINE));
  const auto visible_count = std::max<sptr_t>(
      1, static_cast<sptr_t>(sci(state.editor, SCI_LINESONSCREEN)));
  const auto first_document = std::clamp<sptr_t>(
      static_cast<sptr_t>(
          sci(state.editor, SCI_DOCLINEFROMVISIBLE, first_visible)),
      0, line_count - 1);
  const auto last_document = std::clamp<sptr_t>(
      static_cast<sptr_t>(sci(
          state.editor, SCI_DOCLINEFROMVISIBLE,
          static_cast<WPARAM>(first_visible + visible_count - 1))),
      first_document, line_count - 1);

  int top = client.top + static_cast<int>(
      static_cast<long double>(first_document) * height / line_count);
  int bottom = client.top + static_cast<int>(std::ceil(
      static_cast<long double>(last_document + 1) * height / line_count));
  bottom = std::max(bottom, top + 3);
  if (bottom > client.bottom) {
    top = std::max(client.top, client.bottom - (bottom - top));
    bottom = client.bottom;
  }
  RECT viewport{client.left, top, client.right, bottom};
  const COLORREF fill =
      state.dark ? RGB(62, 95, 135) : RGB(190, 215, 245);
  const COLORREF border =
      state.dark ? RGB(125, 170, 225) : RGB(62, 95, 135);
  alpha_fill(dc, viewport, fill, 80);
  HBRUSH frame = CreateSolidBrush(border);
  if (frame) {
    FrameRect(dc, &viewport, frame);
    DeleteObject(frame);
  }
}

void paint(const HWND window, State& state) {
  PAINTSTRUCT paint{};
  HDC target = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  ensure_preview(target, client, state);
  HDC buffer = CreateCompatibleDC(target);
  HBITMAP bitmap = buffer ? CreateCompatibleBitmap(
                                target, std::max(1L, client.right - client.left),
                                std::max(1L, client.bottom - client.top))
                          : nullptr;
  HGDIOBJ previous_bitmap = nullptr;
  HDC destination = target;
  if (buffer && bitmap) {
    previous_bitmap = SelectObject(buffer, bitmap);
    destination = buffer;
  }

  if (state.preview) {
    HDC preview_dc = CreateCompatibleDC(target);
    if (preview_dc) {
      const HGDIOBJ previous = SelectObject(preview_dc, state.preview);
      BitBlt(destination, client.left, client.top,
             client.right - client.left, client.bottom - client.top,
             preview_dc, 0, 0, SRCCOPY);
      SelectObject(preview_dc, previous);
      DeleteDC(preview_dc);
    }
  } else {
    SetDCBrushColor(destination,
                    state.dark ? RGB(30, 30, 30)
                               : GetSysColor(COLOR_WINDOW));
    FillRect(destination, &client,
             static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  }
  draw_viewport(destination, client, state);
  if (destination == buffer) {
    BitBlt(target, client.left, client.top, client.right - client.left,
           client.bottom - client.top, buffer, client.left, client.top,
           SRCCOPY);
  }

  if (previous_bitmap) SelectObject(buffer, previous_bitmap);
  if (bitmap) DeleteObject(bitmap);
  if (buffer) DeleteDC(buffer);
  EndPaint(window, &paint);
}

LRESULT CALLBACK window_proc(const HWND window, const UINT message,
                             const WPARAM wparam, const LPARAM lparam) {
  State* state = state_for(window);
  if (message == WM_NCCREATE) {
    state = new State;
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(state));
  } else if (message == WM_NCDESTROY) {
    if (state) discard_preview(*state);
    delete state;
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
  } else if (message == WM_SIZE && state) {
    discard_preview(*state);
    InvalidateRect(window, nullptr, FALSE);
    return 0;
  } else if (message == WM_ERASEBKGND) {
    return 1;
  } else if (message == WM_PAINT && state) {
    paint(window, *state);
    return 0;
  } else if (message == WM_SETFOCUS && state && state->editor) {
    SetFocus(state->editor);
    return 0;
  } else if (message == WM_MOUSEACTIVATE) {
    return MA_NOACTIVATE;
  } else if (message == WM_LBUTTONDOWN && state) {
    state->dragging = true;
    SetCapture(window);
    navigate_to_point(window, *state, GET_Y_LPARAM(lparam));
    return 0;
  } else if (message == WM_MOUSEMOVE && state && state->dragging &&
             (wparam & MK_LBUTTON)) {
    navigate_to_point(window, *state, GET_Y_LPARAM(lparam));
    return 0;
  } else if (message == WM_LBUTTONUP && state && state->dragging) {
    state->dragging = false;
    if (GetCapture() == window) ReleaseCapture();
    navigate_to_point(window, *state, GET_Y_LPARAM(lparam));
    return 0;
  } else if (message == WM_CAPTURECHANGED && state) {
    state->dragging = false;
  } else if (message == WM_MOUSEWHEEL && state && state->editor) {
    SendMessageW(state->editor, message, wparam, lparam);
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool register_class(const HINSTANCE instance) {
  WNDCLASSEXW type{sizeof(type)};
  type.hInstance = instance;
  type.lpszClassName = kClassName;
  type.lpfnWndProc = window_proc;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  return RegisterClassExW(&type) != 0 ||
         GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

HWND DocumentMap::create(const HWND parent, const int control_id,
                         const HINSTANCE instance) {
  if (!register_class(instance)) return nullptr;
  return CreateWindowExW(
      0, kClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_id)), instance,
      nullptr);
}

void DocumentMap::attach(const HWND map, const HWND editor) {
  if (!map || !editor) return;
  if (State* state = state_for(map)) {
    state->editor = editor;
    discard_preview(*state);
  }
  InvalidateRect(map, nullptr, FALSE);
}

void DocumentMap::restyle(const HWND map, const HWND,
                          const std::string_view, const bool dark) {
  if (!map) return;
  if (State* state = state_for(map)) {
    state->dark = dark;
    discard_preview(*state);
  }
  InvalidateRect(map, nullptr, FALSE);
}

void DocumentMap::content_changed(const HWND map, const HWND editor) {
  if (!map || !editor) return;
  if (State* state = state_for(map)) {
    state->editor = editor;
    discard_preview(*state);
  }
  InvalidateRect(map, nullptr, FALSE);
}

void DocumentMap::sync(const HWND map, const HWND editor) {
  if (!map || !editor) return;
  if (State* state = state_for(map)) state->editor = editor;
  InvalidateRect(map, nullptr, FALSE);
}

}  // namespace listopad::app
