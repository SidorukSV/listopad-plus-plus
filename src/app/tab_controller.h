#pragma once

#include "document_session.h"

#include <cstddef>
#include <vector>

namespace listopad::app {

class TabController final {
 public:
  using Container = std::vector<DocumentSession>;
  using iterator = Container::iterator;
  using const_iterator = Container::const_iterator;

  [[nodiscard]] bool empty() const noexcept { return sessions_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return sessions_.size(); }
  [[nodiscard]] int active_index() const noexcept { return active_index_; }
  [[nodiscard]] DocumentSession* active() noexcept;
  [[nodiscard]] const DocumentSession* active() const noexcept;

  [[nodiscard]] bool activate(int index) noexcept;
  void push_back(DocumentSession session);
  void pop_back();
  void clear() noexcept;
  iterator erase(iterator position);

  DocumentSession& operator[](std::size_t index) noexcept {
    return sessions_[index];
  }
  const DocumentSession& operator[](std::size_t index) const noexcept {
    return sessions_[index];
  }
  DocumentSession& back() noexcept { return sessions_.back(); }
  const DocumentSession& back() const noexcept { return sessions_.back(); }
  iterator begin() noexcept { return sessions_.begin(); }
  const_iterator begin() const noexcept { return sessions_.begin(); }
  iterator end() noexcept { return sessions_.end(); }
  const_iterator end() const noexcept { return sessions_.end(); }

 private:
  Container sessions_;
  int active_index_{-1};
};

}  // namespace listopad::app
