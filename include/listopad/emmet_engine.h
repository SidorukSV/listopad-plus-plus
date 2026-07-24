#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

struct EmmetField {
  unsigned index{0};
  std::size_t start{0};
  std::size_t length{0};
};

struct EmmetExpansion {
  bool ok{false};
  std::string text;
  std::vector<EmmetField> fields;
  std::string error;
};

class EmmetEngine final {
 public:
  EmmetEngine();
  ~EmmetEngine();
  EmmetEngine(const EmmetEngine&) = delete;
  EmmetEngine& operator=(const EmmetEngine&) = delete;

  EmmetExpansion expand(std::string_view abbreviation,
                        std::string_view syntax);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

bool update_emmet_fields(std::vector<EmmetField>& fields,
                         std::size_t active_index, std::size_t position,
                         std::size_t length, bool insertion);

}  // namespace listopad
