#include "listopad/language.h"

#include "listopad/strings.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace listopad {
namespace {

const std::array<LanguageInfo, 24> kLanguages{{
    {"text", "null", "Plain text", false, false, ".txt", {".txt", ".log"}},
    {"cpp", "cpp", "C / C++", false, false, ".cpp",
     {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}},
    {"csharp", "cpp", "C#", false, false, ".cs", {".cs"}},
    {"css", "css", "CSS", false, true, ".css", {".css"}},
    {"scss", "css", "SCSS", false, true, ".scss", {".scss", ".sass"}},
    {"html", "hypertext", "HTML", true, false, ".html", {".htm", ".html", ".xhtml"}},
    {"xml", "xml", "XML", true, false, ".xml", {".xml", ".xsd", ".xsl", ".svg"}},
    {"javascript", "cpp", "JavaScript", false, false, ".js", {".js", ".mjs", ".cjs"}},
    {"jsx", "hypertext", "JavaScript JSX", true, false, ".jsx", {".jsx"}},
    {"typescript", "cpp", "TypeScript", false, false, ".ts", {".ts"}},
    {"tsx", "hypertext", "TypeScript JSX", true, false, ".tsx", {".tsx"}},
    {"json", "json", "JSON", false, false, ".json", {".json", ".jsonc"}},
    {"python", "python", "Python", false, false, ".py", {".py", ".pyw"}},
    {"rust", "rust", "Rust", false, false, ".rs", {".rs"}},
    {"sql", "sql", "SQL", false, false, ".sql", {".sql"}},
    {"markdown", "markdown", "Markdown", false, false, ".md", {".md", ".markdown"}},
    {"powershell", "powershell", "PowerShell", false, false, ".ps1", {".ps1", ".psm1"}},
    {"bsl", "bsl", "1C:Enterprise (BSL)", false, false, ".bsl", {".bsl"}},
    {"onescript", "bsl", "OneScript", false, false, ".os", {".os"}},
    {"shell", "bash", "Shell", false, false, ".sh", {".sh", ".bash"}},
    {"batch", "batch", "Batch", false, false, ".cmd", {".cmd", ".bat"}},
    {"yaml", "yaml", "YAML", false, false, ".yaml", {".yml", ".yaml"}},
    {"ini", "props", "INI / properties", false, false, ".ini", {".ini", ".properties"}},
    {"diff", "diff", "Diff", false, false, ".diff", {".diff", ".patch"}},
}};

}  // namespace

std::span<const LanguageInfo> languages() { return kLanguages; }

const LanguageInfo* language_by_id(const std::string_view id) {
  const auto it = std::find_if(kLanguages.begin(), kLanguages.end(),
                               [id](const LanguageInfo& item) { return item.id == id; });
  return it == kLanguages.end() ? nullptr : &*it;
}

const LanguageInfo* language_by_extension(std::string_view extension) {
  std::string lower(extension);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (!lower.empty() && lower.front() != '.') lower.insert(lower.begin(), '.');
  const auto language = std::find_if(kLanguages.begin(), kLanguages.end(),
      [&lower](const LanguageInfo& item) {
        return std::find(item.extensions.begin(), item.extensions.end(), lower) !=
               item.extensions.end();
      });
  return language == kLanguages.end() ? nullptr : &*language;
}

std::filesystem::path append_default_extension(
    const std::filesystem::path& path, const std::string_view language_id) {
  if (!path.extension().empty()) return path;
  const std::wstring filename = path.filename().wstring();
  if (filename.size() > 1 && filename.front() == L'.') return path;
  const LanguageInfo* language = language_by_id(language_id);
  if (!language || language->default_extension.empty()) return path;
  std::filesystem::path result = path;
  result += utf8_to_wide(language->default_extension);
  return result;
}

LanguageInfo detect_language(const std::filesystem::path& path,
                             const std::string_view first_line) {
  const std::wstring filename = lowercase(path.filename().wstring());
  if (filename == L"makefile" || filename == L"dockerfile") {
    return *language_by_id("shell");
  }
  if (const LanguageInfo* found =
          language_by_extension(wide_to_utf8(path.extension().wstring()))) {
    return *found;
  }

  if (first_line.starts_with("#!")) {
    if (first_line.find("python") != std::string_view::npos) return *language_by_id("python");
    if (first_line.find("bash") != std::string_view::npos ||
        first_line.find("/sh") != std::string_view::npos) return *language_by_id("shell");
    if (first_line.find("oscript") != std::string_view::npos)
      return *language_by_id("onescript");
  }
  return *language_by_id("text");
}

}  // namespace listopad
