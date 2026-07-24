#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace listopad {

class DocumentMapGeometry final {
 public:
  DocumentMapGeometry(const int height, const std::size_t line_count) noexcept
      : height_(std::max(1, height)),
        line_count_(std::max<std::size_t>(1, line_count)),
        compressed_(line_count_ > static_cast<std::size_t>(height_)),
        line_height_(
            compressed_
                ? 0
                : std::clamp(
                      height_ / static_cast<int>(line_count_), 1,
                      kMaximumPreviewLineHeight)) {}

  [[nodiscard]] bool compressed() const noexcept { return compressed_; }
  [[nodiscard]] int line_height() const noexcept { return line_height_; }

  [[nodiscard]] int content_height() const noexcept {
    if (compressed_) return height_;
    return static_cast<int>(std::min<std::size_t>(
        static_cast<std::size_t>(height_), line_count_ * line_height_));
  }

  [[nodiscard]] int line_top(std::size_t line) const noexcept {
    line = std::min(line, line_count_ - 1);
    if (!compressed_) {
      return static_cast<int>(line * line_height_);
    }
    return static_cast<int>(
        static_cast<long double>(line) * height_ / line_count_);
  }

  [[nodiscard]] int line_bottom(std::size_t line) const noexcept {
    line = std::min(line, line_count_ - 1);
    if (!compressed_) {
      return static_cast<int>(std::min<std::size_t>(
          static_cast<std::size_t>(height_), (line + 1) * line_height_));
    }
    return std::min(
        height_, static_cast<int>(std::ceil(
                     static_cast<long double>(line + 1) * height_ /
                     line_count_)));
  }

  [[nodiscard]] std::size_t line_at(const int y) const noexcept {
    const int clamped_y = std::clamp(y, 0, height_ - 1);
    if (clamped_y == height_ - 1) return line_count_ - 1;
    if (!compressed_) {
      return std::min(line_count_ - 1,
                      static_cast<std::size_t>(clamped_y / line_height_));
    }
    return std::min(
        line_count_ - 1,
        static_cast<std::size_t>(
            static_cast<long double>(clamped_y) * line_count_ / height_));
  }

 private:
  static constexpr int kMaximumPreviewLineHeight = 4;

  int height_;
  std::size_t line_count_;
  bool compressed_;
  int line_height_;
};

}  // namespace listopad
