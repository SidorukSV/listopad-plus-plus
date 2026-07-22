#include "listopad/language.h"

#include "listopad/strings.h"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace listopad {
namespace {

const std::array<LanguageInfo, 24> kLanguages{{
    {"text", "null", "Plain text", false, false},
    {"cpp", "cpp", "C / C++", false, false},
    {"csharp", "cpp", "C#", false, false},
    {"css", "css", "CSS", false, true},
    {"scss", "css", "SCSS", false, true},
    {"html", "hypertext", "HTML", true, false},
    {"xml", "xml", "XML", true, false},
    {"javascript", "cpp", "JavaScript", false, false},
    {"jsx", "hypertext", "JavaScript JSX", true, false},
    {"typescript", "cpp", "TypeScript", false, false},
    {"tsx", "hypertext", "TypeScript JSX", true, false},
    {"json", "json", "JSON", false, false},
    {"python", "python", "Python", false, false},
    {"rust", "rust", "Rust", false, false},
    {"sql", "sql", "SQL", false, false},
    {"markdown", "markdown", "Markdown", false, false},
    {"powershell", "powershell", "PowerShell", false, false},
    {"bsl", "bsl", "1C:Enterprise (BSL)", false, false},
    {"onescript", "bsl", "OneScript", false, false},
    {"shell", "bash", "Shell", false, false},
    {"batch", "batch", "Batch", false, false},
    {"yaml", "yaml", "YAML", false, false},
    {"ini", "props", "INI / properties", false, false},
    {"diff", "diff", "Diff", false, false},
}};

const std::unordered_map<std::wstring, std::string> kExtensions{
    {L".c", "cpp"},       {L".cc", "cpp"},       {L".cpp", "cpp"},
    {L".cxx", "cpp"},     {L".h", "cpp"},        {L".hpp", "cpp"},
    {L".cs", "csharp"},   {L".css", "css"},      {L".scss", "scss"},
    {L".sass", "scss"},   {L".htm", "html"},     {L".html", "html"},
    {L".xhtml", "html"},  {L".xml", "xml"},      {L".xsd", "xml"},
    {L".xsl", "xml"},     {L".svg", "xml"},      {L".js", "javascript"},
    {L".mjs", "javascript"}, {L".cjs", "javascript"}, {L".jsx", "jsx"},
    {L".ts", "typescript"}, {L".tsx", "tsx"},    {L".json", "json"},
    {L".jsonc", "json"},  {L".py", "python"},    {L".pyw", "python"},
    {L".rs", "rust"},     {L".sql", "sql"},      {L".md", "markdown"},
    {L".markdown", "markdown"}, {L".ps1", "powershell"},
    {L".psm1", "powershell"}, {L".sh", "shell"}, {L".bash", "shell"},
    {L".bsl", "bsl"},     {L".os", "onescript"},
    {L".cmd", "batch"},   {L".bat", "batch"},    {L".yml", "yaml"},
    {L".yaml", "yaml"},   {L".ini", "ini"},      {L".properties", "ini"},
    {L".diff", "diff"},   {L".patch", "diff"},
};

}  // namespace

std::span<const LanguageInfo> languages() { return kLanguages; }

const LanguageInfo* language_by_id(const std::string_view id) {
  const auto it = std::find_if(kLanguages.begin(), kLanguages.end(),
                               [id](const LanguageInfo& item) { return item.id == id; });
  return it == kLanguages.end() ? nullptr : &*it;
}

LanguageInfo detect_language(const std::filesystem::path& path,
                             const std::string_view first_line) {
  const std::wstring filename = lowercase(path.filename().wstring());
  if (filename == L"makefile" || filename == L"dockerfile") {
    return *language_by_id("shell");
  }
  const auto found = kExtensions.find(lowercase(path.extension().wstring()));
  if (found != kExtensions.end()) return *language_by_id(found->second);

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
