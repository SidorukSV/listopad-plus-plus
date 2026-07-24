#pragma once

#include <windows.h>

#include <cstddef>
#include <string_view>

namespace listopad::app {

class DocumentMap final {
 public:
  static HWND create(HWND parent, int control_id, HINSTANCE instance);
  static void attach(HWND map, HWND editor);
  static void restyle(HWND map, HWND editor, std::string_view font_face,
                      bool dark);
  static void content_changed(HWND map, HWND editor);
  static void content_changed_from_line(HWND map, HWND editor,
                                        std::size_t first_line);
  static void sync(HWND map, HWND editor);
};

}  // namespace listopad::app
