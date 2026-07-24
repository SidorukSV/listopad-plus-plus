#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace listopad {

struct DocumentMapRun {
  std::uint16_t first_column{0};
  std::uint16_t last_column{0};
  std::uint8_t style{0};

  bool operator==(const DocumentMapRun&) const = default;
};

using DocumentMapLine = std::vector<DocumentMapRun>;

class DocumentMapLineCache final {
 public:
  void reset(std::size_t line_count);
  void invalidate_from(std::size_t first_line, std::size_t line_count);

  [[nodiscard]] std::size_t line_count() const noexcept;
  [[nodiscard]] std::size_t cached_line_count() const noexcept;
  [[nodiscard]] const DocumentMapLine* line(std::size_t index) const noexcept;
  void store(std::size_t index, DocumentMapLine line);

 private:
  std::size_t line_count_{0};
  std::vector<std::pair<std::size_t, DocumentMapLine>> lines_;
};

}  // namespace listopad
