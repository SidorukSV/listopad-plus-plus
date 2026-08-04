#pragma once

#include "listopad/settings.h"

#include <windows.h>

#include <functional>

namespace listopad::app {

struct SettingsActions {
  bool clear_recovery{false};
  bool clear_session{false};
  bool clear_history{false};
};

class SettingsDialog final {
 public:
  using ApplyCallback =
      std::function<bool(const Settings&, const SettingsActions&)>;

  static void show(HWND owner, HINSTANCE instance, const Settings& initial,
                   ApplyCallback apply);
};

}  // namespace listopad::app
