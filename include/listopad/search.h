#pragma once

#include <cstddef>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace listopad {

struct SearchOptions {
  bool regular_expression{false};
  bool match_case{false};
  bool whole_word{false};
};

struct SearchMatch {
  std::size_t start{0};
  std::size_t length{0};
};

struct SearchResult {
  bool ok{false};
  std::vector<SearchMatch> matches;
  std::string error;
};

struct SearchOneResult {
  bool ok{false};
  bool found{false};
  SearchMatch match;
  std::string error;
};

struct ReplaceResult {
  bool ok{false};
  std::string text;
  std::size_t replacements{0};
  std::string error;
};

SearchResult search_all(std::string_view subject, std::string_view pattern,
                        const SearchOptions& options,
                        std::stop_token stop = {});
SearchOneResult search_next(std::string_view subject, std::string_view pattern,
                            const SearchOptions& options, std::size_t start,
                            bool wrap, std::stop_token stop = {});
ReplaceResult replace_all(std::string_view subject, std::string_view pattern,
                          std::string_view replacement,
                          const SearchOptions& options);
ReplaceResult replace_all(std::string_view subject, std::string_view pattern,
                          std::string_view replacement,
                          const SearchOptions& options,
                          std::stop_token stop);

}  // namespace listopad
