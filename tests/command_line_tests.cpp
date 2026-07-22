#include "listopad/command_line.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("command line parses locations, encodings, and Unicode paths") {
  const auto parsed = listopad::parse_command_line(
      {L"ListopadPP.exe", L"--line", L"42:7", L"--encoding", L"windows-1251", L"C:\\тест файл.txt"});
  REQUIRE(parsed.line == 42);
  REQUIRE(parsed.column == 7);
  REQUIRE(parsed.encoding == "windows-1251");
  REQUIRE(parsed.files.size() == 1);
  CHECK(parsed.files.front() == L"C:\\тест файл.txt");
}

TEST_CASE("double dash ends option processing") {
  const auto parsed = listopad::parse_command_line({L"app", L"--", L"--literal.txt"});
  REQUIRE(parsed.files.size() == 1);
  CHECK(parsed.files.front() == L"--literal.txt");
}

