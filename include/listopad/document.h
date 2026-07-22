#pragma once

#include "listopad/encoding.h"
#include "listopad/file_io.h"

#include <filesystem>
#include <optional>
#include <string>

namespace listopad {

struct Document {
  std::filesystem::path path;
  std::wstring title{L"Untitled"};
  std::string text;
  Encoding encoding{};
  EolMode eol{EolMode::CrLf};
  FileFingerprint fingerprint{};
  std::string language{"text"};
  bool dirty{false};
  bool external_diverged{false};
  bool large_file{false};
  bool likely_binary{false};

  [[nodiscard]] bool has_path() const noexcept { return !path.empty(); }
};

struct LoadDocumentResult {
  bool ok{false};
  Document document;
  unsigned long error{0};
};

LoadDocumentResult load_document(const std::filesystem::path& path,
                                 std::uint64_t large_file_threshold,
                                 const Encoding* forced = nullptr);

}  // namespace listopad

