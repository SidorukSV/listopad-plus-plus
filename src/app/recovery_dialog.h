#pragma once

#include "listopad/session.h"

#include <windows.h>

#include <string>
#include <vector>

namespace listopad::app {

enum class RecoveryAction {
  RestoreSelected,
  DeleteAll,
  ExitPreserve,
};

struct RecoveryChoice {
  RecoveryAction action{RecoveryAction::ExitPreserve};
  std::vector<std::string> selected_ids;
};

class RecoveryDialog final {
 public:
  static RecoveryChoice show(
      HWND owner, HINSTANCE instance,
      const std::vector<RecoverySnapshot>& snapshots, bool russian);
};

}  // namespace listopad::app
