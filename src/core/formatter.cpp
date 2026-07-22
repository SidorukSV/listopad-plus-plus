#include "listopad/formatter.h"

#include <pugixml.hpp>
#include <tidy.h>
#include <tidybuffio.h>
#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace listopad {
namespace {

std::pair<std::size_t, std::size_t> line_column(const std::string_view text,
                                                 const std::size_t offset) {
  std::size_t line = 1, column = 1;
  for (std::size_t i = 0; i < std::min(offset, text.size()); ++i) {
    if (text[i] == '\n') { ++line; column = 1; }
    else { ++column; }
  }
  return {line, column};
}

std::string normalize_eol(std::string input, const std::string_view eol) {
  std::string result;
  result.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '\r') {
      if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
      result.append(eol);
    } else if (input[i] == '\n') {
      result.append(eol);
    } else {
      result.push_back(input[i]);
    }
  }
  return result;
}

struct UnknownHtmlTags {
  std::vector<std::string> names;
};

Bool TIDY_CALL collect_unknown_html_tag(TidyMessage message) {
  const char* key = tidyGetMessageKey(message);
  if (!key || (std::strcmp(key, "UNKNOWN_ELEMENT") != 0 &&
               std::strcmp(key, "UNKNOWN_ELEMENT_LOOKS_CUSTOM") != 0)) {
    return no;
  }

  auto* tags = static_cast<UnknownHtmlTags*>(tidyGetAppData(tidyGetMessageDoc(message)));
  if (!tags) return no;

  TidyIterator iterator = tidyGetMessageArguments(message);
  while (iterator) {
    TidyMessageArgument argument = tidyGetNextMessageArgument(message, &iterator);
    if (tidyGetArgType(message, &argument) != tidyFormatType_STRING) continue;
    const char* value = tidyGetArgValueString(message, &argument);
    if (!value) break;

    std::string name(value);
    const std::size_t begin = name.find_first_not_of("</ \t\r\n");
    if (begin == std::string::npos) break;
    const std::size_t end = name.find_first_of(" />\t\r\n", begin);
    name = name.substr(begin, end == std::string::npos ? end : end - begin);
    if (!name.empty() &&
        std::find(tags->names.begin(), tags->names.end(), name) == tags->names.end()) {
      tags->names.push_back(std::move(name));
    }
    break;
  }
  return no;
}

std::string find_custom_html_tags(const std::string_view input) {
  TidyDoc probe = tidyCreate();
  if (!probe) return {};

  UnknownHtmlTags tags;
  tidySetAppData(probe, &tags);
  tidySetMessageCallback(probe, collect_unknown_html_tag);
  tidySetCharEncoding(probe, "utf8");
  tidyOptSetInt(probe, TidyUseCustomTags, TidyCustomBlocklevel);
  tidyOptSetBool(probe, TidyQuiet, yes);
  tidyParseString(probe, std::string(input).c_str());
  tidyRelease(probe);

  std::string joined;
  for (const std::string& name : tags.names) {
    if (!joined.empty()) joined.push_back(',');
    joined.append(name);
  }
  return joined;
}

FormatResult format_json(const std::string_view input, const FormatOptions& options) {
  FormatResult result;
  yyjson_read_err read_error{};
  yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(input.data()), input.size(), 0, nullptr, &read_error);
  if (!doc) {
    result.error = read_error.msg ? read_error.msg : "Invalid JSON";
    const auto [line, column] = line_column(input, read_error.pos);
    result.line = line; result.column = column;
    return result;
  }
  yyjson_write_err write_error{};
  std::size_t length = 0;
  char* output = yyjson_write_opts(doc, YYJSON_WRITE_PRETTY | YYJSON_WRITE_NEWLINE_AT_END,
                                   nullptr, &length, &write_error);
  yyjson_doc_free(doc);
  if (!output) {
    result.error = write_error.msg ? write_error.msg : "Unable to write JSON";
    return result;
  }
  result.text.assign(output, length);
  std::free(output);

  if (options.indent != "    ") {
    std::string adjusted;
    std::size_t line_start = 0;
    while (line_start < result.text.size()) {
      const std::size_t newline = result.text.find('\n', line_start);
      const std::size_t line_end = newline == std::string::npos ? result.text.size() : newline;
      std::size_t spaces = 0;
      while (line_start + spaces < line_end && result.text[line_start + spaces] == ' ') ++spaces;
      adjusted.append(spaces / 4, '\x01');
      adjusted.append(result.text, line_start + spaces, line_end - (line_start + spaces));
      if (newline == std::string::npos) break;
      adjusted.push_back('\n');
      line_start = newline + 1;
    }
    std::string expanded;
    for (const char ch : adjusted) {
      if (ch == '\x01') expanded.append(options.indent);
      else expanded.push_back(ch);
    }
    result.text = std::move(expanded);
  }
  result.text = normalize_eol(std::move(result.text), options.eol);
  result.ok = true;
  return result;
}

FormatResult format_xml(const std::string_view input, const FormatOptions& options) {
  FormatResult result;
  pugi::xml_document document;
  const pugi::xml_parse_result parsed = document.load_buffer(
      input.data(), input.size(), pugi::parse_default | pugi::parse_ws_pcdata,
      pugi::encoding_utf8);
  if (!parsed) {
    result.error = parsed.description();
    const auto [line, column] = line_column(input, static_cast<std::size_t>(parsed.offset));
    result.line = line; result.column = column;
    return result;
  }
  const bool had_declaration = input.find("<?xml") != std::string_view::npos;
  std::ostringstream stream;
  unsigned flags = pugi::format_default;
  if (!had_declaration) flags |= pugi::format_no_declaration;
  document.save(stream, options.indent.c_str(), flags, pugi::encoding_utf8);
  result.text = normalize_eol(stream.str(), options.eol);
  result.ok = true;
  return result;
}

FormatResult format_html(const std::string_view input, const FormatOptions& options) {
  FormatResult result;
  TidyDoc document = tidyCreate();
  if (!document) { result.error = "Unable to create HTML formatter"; return result; }
  TidyBuffer errors{};
  TidyBuffer output{};
  tidyBufInit(&errors); tidyBufInit(&output);
  tidySetErrorBuffer(document, &errors);
  tidySetCharEncoding(document, "utf8");
  tidyOptSetBool(document, TidyMark, no);
  tidyOptSetBool(document, TidyForceOutput, no);
  tidyOptSetBool(document, TidyMakeClean, no);
  tidyOptSetBool(document, TidyDropEmptyElems, no);
  tidyOptSetInt(document, TidyMergeDivs, TidyNoState);
  tidyOptSetInt(document, TidyMergeSpans, TidyNoState);
  tidyOptSetBool(document, TidyJoinStyles, no);
  tidyOptSetInt(document, TidyUseCustomTags, TidyCustomBlocklevel);
  const std::string custom_tags = find_custom_html_tags(input);
  if (!custom_tags.empty()) tidyOptSetValue(document, TidyBlockTags, custom_tags.c_str());
  tidyOptSetInt(document, TidyIndentContent, TidyAutoState);
  tidyOptSetInt(document, TidyIndentSpaces,
                static_cast<ulong>(options.indent == "\t" ? 1 : options.indent.size()));
  tidyOptSetInt(document, TidyWrapLen, 0);
  tidyOptSetInt(document, TidyBodyOnly, TidyAutoState);
  tidyOptSetBool(document, TidyQuiet, yes);
  tidyOptSetBool(document, TidyShowWarnings, no);

  int status = tidyParseString(document, std::string(input).c_str());
  if (status >= 0) status = tidyCleanAndRepair(document);
  if (status >= 0) status = tidyRunDiagnostics(document);
  if (status < 0 || tidyErrorCount(document) > 0) {
    result.error = errors.bp ? reinterpret_cast<const char*>(errors.bp) : "Invalid HTML";
  } else if (tidySaveBuffer(document, &output) < 0) {
    result.error = "Unable to serialize HTML";
  } else {
    result.text.assign(reinterpret_cast<const char*>(output.bp), output.size);
    if (!result.text.empty() && result.text.back() == '\0') result.text.pop_back();
    result.text = normalize_eol(std::move(result.text), options.eol);
    result.ok = true;
  }
  tidyBufFree(&output); tidyBufFree(&errors); tidyRelease(document);
  return result;
}

}  // namespace

FormatResult format_text(const FormatKind kind, const std::string_view input,
                         const FormatOptions& options) {
  switch (kind) {
    case FormatKind::Json: return format_json(input, options);
    case FormatKind::Xml: return format_xml(input, options);
    case FormatKind::Html: return format_html(input, options);
  }
  return {false, {}, "Unsupported formatter"};
}

}  // namespace listopad
