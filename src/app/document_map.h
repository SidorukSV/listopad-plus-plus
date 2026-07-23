#pragma once

#include <windows.h>

#include <string_view>

namespace listopad::app {

class DocumentMap final {
 public:
  static HWND create(HWND parent, int control_id, HINSTANCE instance);
  static void attach(HWND map, HWND editor);
  static void restyle(HWND map, HWND editor, std::string_view font_face,
                      bool dark);
  static void content_changed(HWND map, HWND editor);
  static void sync(HWND map, HWND editor);
};

}  // namespace listopad::app
