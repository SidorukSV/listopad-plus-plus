#include "listopad/document.h"

#include "listopad/language.h"

#include <windows.h>

namespace listopad {

LoadDocumentResult load_document(const std::filesystem::path& input,
                                 const std::uint64_t large_file_threshold,
                                 const Encoding* forced) {
  LoadDocumentResult result;
  const auto path = canonical_path(input);
  const FileFingerprint initial = fingerprint_file(path);
  if (!initial.exists) {
    result.error = ERROR_FILE_NOT_FOUND;
    return result;
  }
  result.document.path = path;
  result.document.title = path.filename().wstring();
  result.document.fingerprint = initial;
  if (initial.size >= large_file_threshold) {
    result.document.large_file = true;
    result.document.language = detect_language(path).id;
    result.ok = true;
    return result;
  }
  const ReadFileResult file = read_file(path);
  if (!file.ok) {
    result.error = file.error;
    return result;
  }
  DecodedText decoded = decode_text(file.bytes, forced);
  result.document.text = std::move(decoded.utf8);
  result.document.encoding = decoded.encoding;
  result.document.eol = decoded.eol;
  result.document.likely_binary = decoded.likely_binary;
  result.document.fingerprint = file.fingerprint;
  const std::size_t end = result.document.text.find_first_of("\r\n");
  result.document.language = detect_language(path, result.document.text.substr(0, end)).id;
  result.ok = true;
  return result;
}

}  // namespace listopad
