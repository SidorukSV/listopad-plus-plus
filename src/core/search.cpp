#include "listopad/search.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <algorithm>
#include <memory>

namespace listopad {
namespace {

struct CodeDeleter { void operator()(pcre2_code* value) const { pcre2_code_free(value); } };
struct MatchDeleter { void operator()(pcre2_match_data* value) const { pcre2_match_data_free(value); } };

std::string escape_pattern(const std::string_view input) {
  constexpr std::string_view special = R"(\.^$|()[]{}*+?)";
  std::string result;
  result.reserve(input.size() * 2);
  for (const char ch : input) {
    if (special.find(ch) != std::string_view::npos) result.push_back('\\');
    result.push_back(ch);
  }
  return result;
}

std::string prepare_pattern(const std::string_view pattern, const SearchOptions& options) {
  std::string result = options.regular_expression ? std::string(pattern) : escape_pattern(pattern);
  if (options.whole_word) result = "(?<![\\p{L}\\p{N}_])(?:" + result + ")(?![\\p{L}\\p{N}_])";
  return result;
}

std::unique_ptr<pcre2_code, CodeDeleter> compile(const std::string_view pattern,
                                                 const SearchOptions& options,
                                                 std::string& error,
                                                 const std::stop_token stop) {
  const std::string prepared = prepare_pattern(pattern, options);
  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  std::uint32_t flags = PCRE2_UTF | PCRE2_UCP | PCRE2_MULTILINE;
  if (stop.stop_possible()) flags |= PCRE2_AUTO_CALLOUT;
  if (!options.match_case) flags |= PCRE2_CASELESS;
  pcre2_code* raw = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(prepared.data()), prepared.size(),
                                  flags, &error_code, &error_offset, nullptr);
  if (!raw) {
    PCRE2_UCHAR message[256]{};
    pcre2_get_error_message(error_code, message, std::size(message));
    error.assign(reinterpret_cast<const char*>(message));
    error += " at offset " + std::to_string(error_offset);
    return {};
  }
  pcre2_jit_compile(raw, PCRE2_JIT_COMPLETE);
  return std::unique_ptr<pcre2_code, CodeDeleter>(raw);
}

int cancel_callout(pcre2_callout_block*, void* data) {
  const auto* stop = static_cast<const std::stop_token*>(data);
  return stop && stop->stop_requested() ? 1 : 0;
}

void configure_context(pcre2_match_context* context, const std::stop_token& stop) {
  pcre2_set_match_limit(context, 10'000'000);
  pcre2_set_depth_limit(context, 10'000);
  pcre2_set_heap_limit(context, 64 * 1024);
  if (stop.stop_possible()) pcre2_set_callout(context, cancel_callout,
                                               const_cast<std::stop_token*>(&stop));
}

std::size_t next_utf8(const std::string_view value, const std::size_t offset) {
  if (offset >= value.size()) return value.size() + 1;
  std::size_t next = offset + 1;
  while (next < value.size() && (static_cast<unsigned char>(value[next]) & 0xc0) == 0x80) ++next;
  return next;
}

}  // namespace

SearchResult search_all(const std::string_view subject, const std::string_view pattern,
                        const SearchOptions& options, const std::stop_token stop) {
  SearchResult result;
  if (pattern.empty()) { result.ok = true; return result; }
  auto code = compile(pattern, options, result.error, stop);
  if (!code) return result;
  std::unique_ptr<pcre2_match_data, MatchDeleter> match(pcre2_match_data_create_from_pattern(code.get(), nullptr));
  pcre2_match_context* context = pcre2_match_context_create(nullptr);
  configure_context(context, stop);

  std::size_t offset = 0;
  while (offset <= subject.size() && !stop.stop_requested()) {
    const int count = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()),
                                  subject.size(), offset, 0, match.get(), context);
    if (count == PCRE2_ERROR_NOMATCH) break;
    if (count < 0) {
      result.error = stop.stop_requested() ? "Search cancelled"
                                           : "PCRE2 match failed: " + std::to_string(count);
      pcre2_match_context_free(context);
      return result;
    }
    const PCRE2_SIZE* vector = pcre2_get_ovector_pointer(match.get());
    result.matches.push_back({static_cast<std::size_t>(vector[0]),
                              static_cast<std::size_t>(vector[1] - vector[0])});
    if (result.matches.size() >= 5'000'000) {
      result.error = "Search result limit exceeded";
      pcre2_match_context_free(context);
      return result;
    }
    offset = vector[1] > vector[0] ? static_cast<std::size_t>(vector[1])
                                   : next_utf8(subject, static_cast<std::size_t>(vector[1]));
  }
  pcre2_match_context_free(context);
  if (stop.stop_requested()) { result.error = "Search cancelled"; return result; }
  result.ok = true;
  return result;
}

SearchOneResult search_next(const std::string_view subject, const std::string_view pattern,
                            const SearchOptions& options, const std::size_t start,
                            const bool wrap, const std::stop_token stop) {
  SearchOneResult result;
  if (pattern.empty()) { result.ok = true; return result; }
  auto code = compile(pattern, options, result.error, stop);
  if (!code) return result;
  std::unique_ptr<pcre2_match_data, MatchDeleter> match(
      pcre2_match_data_create_from_pattern(code.get(), nullptr));
  pcre2_match_context* context = pcre2_match_context_create(nullptr);
  configure_context(context, stop);

  const auto run = [&](const std::size_t offset) {
    return pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()),
                       subject.size(), offset, 0, match.get(), context);
  };
  const std::size_t safe_start = std::min(start, subject.size());
  int count = run(safe_start);
  if (count == PCRE2_ERROR_NOMATCH && wrap && safe_start > 0) count = run(0);
  if (count >= 0) {
    const PCRE2_SIZE* vector = pcre2_get_ovector_pointer(match.get());
    result.match = {static_cast<std::size_t>(vector[0]),
                    static_cast<std::size_t>(vector[1] - vector[0])};
    result.found = true;
    result.ok = true;
  } else if (count == PCRE2_ERROR_NOMATCH) {
    result.ok = true;
  } else {
    result.error = stop.stop_requested() ? "Search cancelled"
                                         : "PCRE2 match failed: " + std::to_string(count);
  }
  pcre2_match_context_free(context);
  return result;
}

ReplaceResult replace_all(const std::string_view subject, const std::string_view pattern,
                          const std::string_view replacement, const SearchOptions& options) {
  return replace_all(subject, pattern, replacement, options, {});
}

ReplaceResult replace_all(const std::string_view subject, const std::string_view pattern,
                          const std::string_view replacement, const SearchOptions& options,
                          const std::stop_token stop) {
  ReplaceResult result;
  if (pattern.empty()) { result.ok = true; result.text = subject; return result; }
  auto code = compile(pattern, options, result.error, stop);
  if (!code) return result;
  pcre2_match_context* context = pcre2_match_context_create(nullptr);
  configure_context(context, stop);
  std::vector<PCRE2_UCHAR> output(std::max<std::size_t>(subject.size() + 1024, 4096));
  for (;;) {
    PCRE2_SIZE output_length = output.size();
    const int count = pcre2_substitute(
        code.get(), reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(), 0,
        PCRE2_SUBSTITUTE_GLOBAL | PCRE2_SUBSTITUTE_EXTENDED | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
        nullptr, context, reinterpret_cast<PCRE2_SPTR>(replacement.data()), replacement.size(),
        output.data(), &output_length);
    if (count == PCRE2_ERROR_NOMEMORY) {
      if (output_length > (512ull << 20)) {
        pcre2_match_context_free(context);
        result.error = "Replacement output limit exceeded";
        return result;
      }
      output.resize(static_cast<std::size_t>(output_length) + 1);
      continue;
    }
    pcre2_match_context_free(context);
    if (count < 0) {
      if (stop.stop_requested()) result.error = "Search cancelled";
      else
      result.error = "PCRE2 replacement failed: " + std::to_string(count);
      return result;
    }
    result.text.assign(reinterpret_cast<const char*>(output.data()), output_length);
    result.replacements = static_cast<std::size_t>(count);
    result.ok = true;
    return result;
  }
}

}  // namespace listopad
