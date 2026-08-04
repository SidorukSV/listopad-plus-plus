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

TEST_CASE("elevated restart handoff is parsed without treating its path as a file") {
  const auto parsed = listopad::parse_command_line(
      {L"ListopadPP.exe", L"--elevated-restart", L"1234",
       L"Local\\ListopadPP.ElevatedRestart.1234.nonce",
       L"0123456789abcdef0123456789abcdef",
       L"C:\\Windows\\System32\\drivers\\etc\\hosts",
       L"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"});

  REQUIRE(parsed.elevated_restart.has_value());
  CHECK(parsed.elevated_restart->parent_process_id == 1234);
  CHECK(parsed.elevated_restart->ready_event ==
        L"Local\\ListopadPP.ElevatedRestart.1234.nonce");
  CHECK(parsed.elevated_restart->tab_id ==
        "0123456789abcdef0123456789abcdef");
  CHECK(parsed.elevated_restart->target_path ==
        L"C:\\Windows\\System32\\drivers\\etc\\hosts");
  CHECK(parsed.elevated_restart->content_sha256 ==
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  CHECK(parsed.files.empty());
}

TEST_CASE("invalid elevated restart handoff is ignored") {
  const auto parsed = listopad::parse_command_line(
      {L"ListopadPP.exe", L"--elevated-restart", L"0", L"event", L"tab",
       L"C:\\Windows\\hosts", L"not-a-hash"});
  CHECK_FALSE(parsed.elevated_restart.has_value());
  CHECK(parsed.files.empty());
}
