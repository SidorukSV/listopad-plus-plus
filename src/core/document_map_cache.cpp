#include "listopad/document_map_cache.h"

#include <algorithm>
#include <utility>

namespace listopad {

void DocumentMapLineCache::reset(const std::size_t line_count) {
  line_count_ = line_count;
  lines_.clear();
}

void DocumentMapLineCache::invalidate_from(
    const std::size_t first_line, const std::size_t line_count) {
  line_count_ = line_count;
  const auto begin = std::lower_bound(
      lines_.begin(), lines_.end(), first_line,
      [](const auto& entry, const std::size_t line) {
        return entry.first < line;
      });
  lines_.erase(begin, lines_.end());
}

std::size_t DocumentMapLineCache::line_count() const noexcept {
  return line_count_;
}

std::size_t DocumentMapLineCache::cached_line_count() const noexcept {
  return lines_.size();
}

const DocumentMapLine* DocumentMapLineCache::line(
    const std::size_t index) const noexcept {
  const auto entry = std::lower_bound(
      lines_.begin(), lines_.end(), index,
      [](const auto& candidate, const std::size_t line) {
        return candidate.first < line;
      });
  if (entry == lines_.end() || entry->first != index) return nullptr;
  return &entry->second;
}

void DocumentMapLineCache::store(
    const std::size_t index, DocumentMapLine line) {
  if (index >= line_count_) return;

  const auto entry = std::lower_bound(
      lines_.begin(), lines_.end(), index,
      [](const auto& candidate, const std::size_t line_index) {
        return candidate.first < line_index;
      });
  if (entry != lines_.end() && entry->first == index) {
    entry->second = std::move(line);
    return;
  }
  lines_.insert(entry, std::pair{index, std::move(line)});
}

}  // namespace listopad
