#include "listopad/search.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("regexp search supports Unicode and zero length matches") {
  listopad::SearchOptions options{.regular_expression = true, .match_case = false};
  const auto words = listopad::search_all("Привет привет", "привет", options);
  REQUIRE(words.ok);
  CHECK(words.matches.size() == 2);
  const auto empty = listopad::search_all("abc", "(?=.)", options);
  REQUIRE(empty.ok);
  CHECK(empty.matches.size() == 3);
}

TEST_CASE("replacement supports numbered and named groups") {
  listopad::SearchOptions options{.regular_expression = true, .match_case = true};
  const auto result = listopad::replace_all("a=1 b=2", R"((?<name>[a-z])=(\d))", "${name}:$2", options);
  REQUIRE(result.ok);
  CHECK(result.replacements == 2);
  CHECK(result.text == "a:1 b:2");
}

TEST_CASE("plain search escapes metacharacters") {
  const auto result = listopad::search_all("a.b axb", "a.b", {});
  REQUIRE(result.ok);
  REQUIRE(result.matches.size() == 1);
  CHECK(result.matches.front().start == 0);
}

TEST_CASE("search next wraps and cancellable search observes stop requests") {
  listopad::SearchOptions options;
  const listopad::SearchOneResult wrapped = listopad::search_next("one two one", "one", options, 9, true);
  REQUIRE(wrapped.ok);
  REQUIRE(wrapped.found);
  REQUIRE(wrapped.match.start == 0);

  std::stop_source source;
  source.request_stop();
  const listopad::SearchResult cancelled = listopad::search_all(
      std::string(10000, 'a'), "(a+)+$", options, source.get_token());
  REQUIRE_FALSE(cancelled.ok);
  REQUIRE(cancelled.error == "Search cancelled");
}
