#include "listopad/technology_log.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace listopad;

namespace {

constexpr std::string_view kLog =
    "17:48.771000-0,VRSREQUEST,4,level=INFO,process=1cv8c,"
    "OSThread=48850,Method=POST,URI='/e1cib/logForm?cmd=query',"
    "Headers='One: 1\nTwo: 2',Body=0\n"
    "17:49.825000-1053999,SCALL,0,level=INFO,process=1cv8c,"
    "OSThread=49130,IName=IVResourceRemoteConnection,MName=send\n"
    "17:49.825001-0,EXCP,4,level=WARNING,process=1cv8c,"
    "OSThread=48850,Descr='src/component/file.cpp(379):\n"
    "580392e6-ba49-4280-ac67-fcd6f2180121: HTTP: Bad request'\n"
    "17:49.826000-0,VRSCACHE,5,level=INFO,process=1cv8c,"
    "OSThread=48850,Sql=\" SELECT\nx,y FROM table\",Result=hit\n"
    "17:49.827000-0,VRSRESPONSE,4,level=INFO,process=1cv8c,"
    "OSThread=48850,Status=200,Phrase=OK,Body=60126";

}  // namespace

TEST_CASE("technology log parser keeps multiline records and raw source") {
  REQUIRE(looks_like_technology_log(kLog));
  const TechnologyLogDocument document =
      parse_technology_log(kLog, "26072812.log");
  REQUIRE_FALSE(document.cancelled);
  REQUIRE(document.events.size() == 5);

  const TechnologyLogEvent& request = document.events[0];
  CHECK(request.type == "VRSREQUEST");
  CHECK(request.nesting_level == 4);
  CHECK(technology_log_field_value(kLog, request, "headers") ==
        "One: 1\nTwo: 2");
  CHECK(technology_log_raw_event(kLog, request) ==
        kLog.substr(0, kLog.find("\n17:49.825000")));

  const TechnologyLogEvent& cache = document.events[3];
  CHECK(technology_log_field_value(kLog, cache, "Sql") ==
        " SELECT\nx,y FROM table");
  CHECK(technology_log_field_value(kLog, cache, "result") == "hit");
}

TEST_CASE("technology log time and duration use the hourly file name") {
  const TechnologyLogDocument document =
      parse_technology_log(kLog, "26072812.log");
  REQUIRE(document.events.size() > 1);
  CHECK(technology_log_format_timestamp(document, document.events[1]) ==
        "2026-07-28 12:17:49.825000");
  CHECK(technology_log_format_duration(document.events[1].duration_us, false) ==
        "1.054 s");
  CHECK(technology_log_format_duration(document.events[1].duration_us, true) ==
        "1,054 с");
}

TEST_CASE("technology log interpretation remains deterministic") {
  const TechnologyLogDocument document =
      parse_technology_log(kLog, "26072812.log");
  REQUIRE(document.events.size() == 5);
  CHECK(interpret_technology_log_event(kLog, document.events[0], true)
            .summary ==
        "HTTP-запрос POST /e1cib/logForm?cmd=query");
  CHECK(interpret_technology_log_event(kLog, document.events[1], true)
            .summary ==
        "Серверный вызов IVResourceRemoteConnection.send · 1,054 с");
  const auto exception =
      interpret_technology_log_event(kLog, document.events[2], true);
  CHECK(exception.kind == TechnologyLogInterpretationKind::Error);
  CHECK(exception.summary == "Исключение: HTTP: Bad request");
  CHECK(interpret_technology_log_event(kLog, document.events[3], true)
            .summary ==
        "Запрос к кэшу VRS: SELECT · hit");
}

TEST_CASE("technology log interpretation promotes protocol failures") {
  constexpr std::string_view source =
      "00:00.000001-0,VRSRESPONSE,0,level=INFO,process=1cv8c,"
      "Status=503,Phrase='Service unavailable'\n"
      "00:00.000002-0,LIC,0,level=INFO,process=1cv8c,"
      "Func=getLicense,res=error";
  const TechnologyLogDocument document =
      parse_technology_log(source, "26072812.log");
  REQUIRE(document.events.size() == 2);
  CHECK(interpret_technology_log_event(source, document.events[0], true)
            .kind == TechnologyLogInterpretationKind::Error);
  const auto license =
      interpret_technology_log_event(source, document.events[1], true);
  CHECK(license.kind == TechnologyLogInterpretationKind::Warning);
  CHECK(license.summary == "Лицензирование: getLicense · error");
}

TEST_CASE("technology log parser reports an unterminated final value") {
  constexpr std::string_view source =
      "00:00.000001-0,EXCP,0,level=WARNING,process=1cv8c,"
      "Descr='unfinished";
  const TechnologyLogDocument document =
      parse_technology_log(source, "26072812.log");
  REQUIRE(document.events.size() == 1);
  CHECK(document.events[0].incomplete);
  CHECK(technology_log_raw_event(source, document.events[0]) == source);
}

TEST_CASE("technology log parser observes a pre-requested stop") {
  std::stop_source stop;
  stop.request_stop();
  const TechnologyLogDocument document =
      parse_technology_log(kLog, "26072812.log", stop.get_token());
  CHECK(document.cancelled);
  CHECK(document.events.empty());
}

TEST_CASE("technology log detection rejects ordinary log text") {
  CHECK_FALSE(looks_like_technology_log(
      "2026-07-28 12:17:49 INFO application started\n"));
}

TEST_CASE("technology log layout exposes raw source only on request") {
  const TechnologyLogLayout hidden =
      calculate_technology_log_layout(1000, 700, 96, false);
  CHECK(hidden.raw.height == 0);
  CHECK(hidden.details.height > 0);

  const TechnologyLogLayout visible =
      calculate_technology_log_layout(1000, 700, 96, true);
  CHECK(visible.raw.height > 0);
  CHECK(visible.details.height < hidden.details.height);
  CHECK(visible.raw.y >= visible.details.y + visible.details.height);
}
