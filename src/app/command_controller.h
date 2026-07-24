#pragma once

namespace listopad::app {

class EditorWindow;

class CommandController final {
 public:
  [[nodiscard]] bool dispatch(EditorWindow& owner, int command) const;
};

}  // namespace listopad::app
