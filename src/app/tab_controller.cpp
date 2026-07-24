#include "tab_controller.h"

#include <algorithm>
#include <utility>

namespace listopad::app {

DocumentSession* TabController::active() noexcept {
  return active_index_ >= 0 ? &sessions_[static_cast<std::size_t>(active_index_)]
                            : nullptr;
}

const DocumentSession* TabController::active() const noexcept {
  return active_index_ >= 0 ? &sessions_[static_cast<std::size_t>(active_index_)]
                            : nullptr;
}

bool TabController::activate(const int index) noexcept {
  if (index < 0 || index >= static_cast<int>(sessions_.size())) return false;
  active_index_ = index;
  return true;
}

void TabController::push_back(DocumentSession session) {
  sessions_.push_back(std::move(session));
  if (active_index_ < 0) active_index_ = 0;
}

void TabController::pop_back() {
  if (sessions_.empty()) return;
  sessions_.pop_back();
  if (sessions_.empty()) {
    active_index_ = -1;
  } else if (active_index_ >= static_cast<int>(sessions_.size())) {
    active_index_ = static_cast<int>(sessions_.size()) - 1;
  }
}

void TabController::clear() noexcept {
  sessions_.clear();
  active_index_ = -1;
}

TabController::iterator TabController::erase(const iterator position) {
  const int erased_index =
      static_cast<int>(std::distance(sessions_.begin(), position));
  const iterator next = sessions_.erase(position);
  if (sessions_.empty()) {
    active_index_ = -1;
  } else if (erased_index < active_index_) {
    --active_index_;
  } else if (erased_index == active_index_) {
    active_index_ =
        (std::min)(erased_index, static_cast<int>(sessions_.size()) - 1);
  }
  return next;
}

}  // namespace listopad::app
