#include "listopad/pff_profile.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <stop_token>
#include <string>

using namespace listopad;

namespace {

const std::string kFixture =
    "\xEF\xBB\xBF{5,2,id,\r\n"
    "{0,0,id,\r\n"
    "{{\"\",0},id,id,0,id,0,"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAA=,\"\"},"
    "\"ОбщийМодуль.Тест.Модуль\",42,"
    "\"Результат = Функция(\"\"тест\"\");\",3,"
    "1.25,0.75,12.5,7.5,1,0,1,id,\r\n"
    "{{\"\",0},id,id,0,id,0,"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAA=,\"Ext\"},"
    "\"Документ.Заказ.МодульОбъекта\",7,\"Возврат;\",1,"
    "0.1,0.05,0.5,0.25,0,1,0,id,";

}  // namespace

TEST_CASE("PFF profile signature is conservative") {
  CHECK(looks_like_pff_profile(kFixture));
  CHECK_FALSE(looks_like_pff_profile("{5,2,not-a-profile}"));
  CHECK_FALSE(looks_like_pff_profile(
      std::string(70 * 1024, 'x') +
      "AAAAAAAAAAAAAAAAAAAAAAAAAAA=,\"\"},\"M\",1,\"x\",1,1,1,1,1"));
}

TEST_CASE("PFF profile parser reads timing and execution context") {
  const PffProfileDocument document = parse_pff_profile(kFixture);
  REQUIRE(document.records.size() == 2);
  CHECK(document.module_count == 2);
  CHECK(document.skipped_records == 0);
  CHECK(document.total_self_time == Catch::Approx(0.8));

  const PffProfileRecord& first = document.records.front();
  CHECK(first.module == "ОбщийМодуль.Тест.Модуль");
  CHECK(first.line == 42);
  CHECK(first.code == "Результат = Функция(\"тест\");");
  CHECK(first.calls == 3);
  CHECK(first.inclusive_time == 1.25);
  CHECK(first.self_time == 0.75);
  CHECK(first.inclusive_share == 12.5);
  CHECK(first.self_share == 7.5);
  CHECK(first.client);
  CHECK_FALSE(first.server);
  CHECK(first.server_call);
  CHECK_FALSE(first.incomplete);
}

TEST_CASE("PFF profile parser observes a pre-requested stop") {
  std::stop_source cancellation;
  cancellation.request_stop();
  const PffProfileDocument document =
      parse_pff_profile(kFixture, cancellation.get_token());
  CHECK(document.cancelled);
  CHECK(document.records.empty());
}

TEST_CASE("PFF filters and formatting are deterministic") {
  PffProfileRecord record;
  record.self_share = 1.25;
  record.client = true;
  CHECK(pff_profile_record_matches_filter(record, PffProfileFilter::Hot));
  CHECK(pff_profile_record_matches_filter(record, PffProfileFilter::Client));
  CHECK_FALSE(
      pff_profile_record_matches_filter(record, PffProfileFilter::Server));
  CHECK(pff_profile_format_time(0.0000125, false) == "12.500 us");
  CHECK(pff_profile_format_time(0.0125, true) == "12,500 мс");
  CHECK(pff_profile_format_share(1.25, true) == "1,250 %");
  CHECK(pff_profile_context(record, true) == "Клиент");
}

TEST_CASE("PFF layout remains inside the client area") {
  const PffProfileLayout layout = calculate_pff_profile_layout(1200, 800, 96);
  CHECK(layout.summary.x >= 0);
  CHECK(layout.filter.x >= layout.summary.x + layout.summary.width);
  CHECK(layout.records.y >= layout.summary.y + layout.summary.height);
  CHECK(layout.records.x + layout.records.width <= 1200);
  CHECK(layout.records.y + layout.records.height <= 800);
}
