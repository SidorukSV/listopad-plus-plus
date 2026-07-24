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
constexpr int kPreviewColumns = 112;

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

void draw_preview_line(const HDC dc, const RECT& client, const HWND editor,
                       const sptr_t line, const int y, const int thickness) {
  const auto line_start =
      static_cast<sptr_t>(sci(editor, SCI_POSITIONFROMLINE, line));
  const auto line_end =
      static_cast<sptr_t>(sci(editor, SCI_GETLINEENDPOSITION, line));
  if (line_start < 0 || line_end <= line_start) return;
  const auto preview_end = std::min<sptr_t>(
      line_end, static_cast<sptr_t>(
                    sci(editor, SCI_FINDCOLUMN, line, kPreviewColumns)));
  const int width = std::max(
      1L, client.right - client.left - kHorizontalPadding * 2);
  sptr_t run_start = -1;
  unsigned char run_style = 0;
  const auto flush_run = [&](const sptr_t run_end) {
    if (run_start < 0 || run_end <= run_start) return;
    const int first_column = std::clamp(
        static_cast<int>(sci(editor, SCI_GETCOLUMN, run_start)), 0,
        kPreviewColumns - 1);
    const int last_column = std::clamp(
        static_cast<int>(sci(editor, SCI_GETCOLUMN, run_end)),
        first_column + 1, kPreviewColumns);
    RECT fragment{
        client.left + kHorizontalPadding +
            MulDiv(first_column, width, kPreviewColumns),
        y,
        client.left + kHorizontalPadding +
            MulDiv(last_column, width, kPreviewColumns),
        y + thickness};
    fragment.right = std::max(fragment.left + 1, fragment.right);
    SetDCBrushColor(
        dc, static_cast<COLORREF>(
                sci(editor, SCI_STYLEGETFORE,
                    static_cast<WPARAM>(run_style))));
    FillRect(dc, &fragment,
             static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  };

  for (sptr_t position = line_start; position < preview_end; ++position) {
    const unsigned char character = static_cast<unsigned char>(
        sci(editor, SCI_GETCHARAT, static_cast<WPARAM>(position)));
    const bool whitespace =
        character == ' ' || character == '\t' || character == '\r' ||
        character == '\n';
    const auto style = static_cast<unsigned char>(
        sci(editor, SCI_GETSTYLEAT, static_cast<WPARAM>(position)));
    if (whitespace) {
      flush_run(position);
      run_start = -1;
    } else if (run_start < 0) {
      run_start = position;
      run_style = style;
    } else if (style != run_style) {
      flush_run(position);
      run_start = position;
      run_style = style;
    }
  }
  flush_run(preview_end);
}

void draw_document_preview(const HDC dc, const RECT& client,
                           const State& state) {
  if (!state.editor) return;
  const int height = std::max(1L, client.bottom - client.top);
  const auto line_count =
      std::max<sptr_t>(1, static_cast<sptr_t>(
                              sci(state.editor, SCI_GETLINECOUNT)));
  if (line_count <= height) {
    for (sptr_t line = 0; line < line_count; ++line) {
      const int top = client.top + static_cast<int>(
          static_cast<long double>(line) * height / line_count);
      const int bottom = client.top + static_cast<int>(
          static_cast<long double>(line + 1) * height / line_count);
      const int thickness = std::clamp(bottom - top - 2, 2, 4);
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
  const COLORREF background = static_cast<COLORREF>(
      sci(state.editor, SCI_STYLEGETBACK, STYLE_DEFAULT));
  SetDCBrushColor(preview_dc, background);
  FillRect(preview_dc, &preview_rect,
           static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  draw_document_preview(preview_dc, preview_rect, state);
  SelectObject(preview_dc, previous);
  DeleteDC(preview_dc);
  state.preview_dirty = false;
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
  const COLORREF border =
      state.dark ? RGB(125, 170, 225) : RGB(62, 95, 135);
  HBRUSH frame = CreateSolidBrush(border);
  if (frame) {
    FrameRect(dc, &viewport, frame);
    InflateRect(&viewport, -1, -1);
    if (viewport.right > viewport.left && viewport.bottom > viewport.top) {
      FrameRect(dc, &viewport, frame);
    }
    DeleteObject(frame);
  }
}

void paint(const HWND window, State& state) {
  PAINTSTRUCT paint{};
  HDC target = BeginPaint(window, &paint);
  RECT client{};
  GetClientRect(window, &client);
  ensure_preview(target, client, state);

  if (state.preview) {
    HDC preview_dc = CreateCompatibleDC(target);
    if (preview_dc) {
      const HGDIOBJ previous = SelectObject(preview_dc, state.preview);
      BitBlt(target, client.left, client.top,
             client.right - client.left, client.bottom - client.top,
             preview_dc, 0, 0, SRCCOPY);
      SelectObject(preview_dc, previous);
      DeleteDC(preview_dc);
    }
  } else {
    SetDCBrushColor(target,
                    state.dark ? RGB(30, 30, 30)
                               : GetSysColor(COLOR_WINDOW));
    FillRect(target, &client,
             static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  }
  // The cached preview is already an off-screen buffer. Drawing the viewport
  // directly over that copy avoids allocating another full-size bitmap for
  // every scroll event.
  draw_viewport(target, client, state);
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
    const int width = LOWORD(lparam);
    const int height = HIWORD(lparam);
    // update_layout() may send WM_SIZE even when the map dimensions did not
    // change. Keep the expensive document preview in that common case.
    if (state->preview_width != width || state->preview_height != height) {
      discard_preview(*state);
    }
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
