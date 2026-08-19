#include "listopad/performance_log.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace listopad;

namespace {

// A trimmed PDH-CSV export: a memory counter that always has a value, a disk
// counter whose first sample is empty, and a process instance that only
// starts reporting after the run began.
constexpr std::string_view kCsv =
    "\"(PDH-CSV 4.0) (Russia TZ 4 Standard Time)(-300)\","
    "\"\\\\1C\\Память\\Доступно МБ\","
    "\"\\\\1C\\Физический диск(0 C:)\\Средняя длина очереди диска\","
    "\"\\\\1C\\Процесс(rphost)\\% загруженности процессора\"\r\n"
    "\"08/13/2026 21:23:46.194\",\"27899\",\" \",\" \"\r\n"
    "\"08/13/2026 21:23:51.198\",\"20480\",\"0.5\",\"12.5\"\r\n"
    "\"08/13/2026 21:23:56.202\",\"100\",\"9.5\",\"37.5\"\r\n";

// A compact slice of the local MonthClose BLG shape: SQL Server counters use
// English object names with Russian counter names, plus process CPU instances.
constexpr std::string_view kSqlCsv =
    "\"(PDH-CSV 4.0) (Russia TZ 4 Standard Time)(-300)\","
    "\"\\\\1C\\SQLServer:Buffer Manager\\Примерный срок хранения страницы\","
    "\"\\\\1C\\SQLServer:Memory Manager\\Ожидается выделений памяти\","
    "\"\\\\1C\\SQLServer:Databases(2stn_erp)\\Время записи журнала на диск (мс)\","
    "\"\\\\1C\\SQLServer:Resource Pool Stats(default)\\Средняя продолжительность операции записи на диск, мс\","
    "\"\\\\1C\\SQLServer:Wait Statistics(Average wait time (ms))\\Ожиданий записи в журнал\","
    "\"\\\\1C\\Процесс(sqlservr)\\% загруженности процессора\"\r\n"
    "\"08/19/2026 00:01:34.126\",\"219\",\"0\",\"2000\",\"134\",\"130\",\"48\"\r\n"
    "\"08/19/2026 00:01:39.166\",\"1000\",\"4\",\"2200\",\"120\",\"140\",\"20\"\r\n";

const PerformanceCounterSeries& series_at(
    const PerformanceLogDocument& document, const std::size_t index) {
  REQUIRE(document.counters.size() > index);
  return document.counters[index];
}

}  // namespace

TEST_CASE("performance log detection accepts only the PDH text signature") {
  CHECK(looks_like_performance_log_text(kCsv));
  CHECK(looks_like_performance_log_text(
      "\"(PDH-TSV 4.0) (GMT)(0)\"\t"
      "\"\\\\host\\Memory\\Available MBytes\"\n"));
  CHECK_FALSE(looks_like_performance_log_text(
      "\"time\",\"value\"\r\n\"08/13/2026 21:23:46.194\",\"1\"\r\n"));
  CHECK(looks_like_performance_log_name("C:/PerfLogs/MonthClose_000001.BLG"));
  CHECK_FALSE(looks_like_performance_log_name("C:/PerfLogs/report.csv"));
}

TEST_CASE("performance log parser keeps counter paths and sample gaps") {
  const PerformanceLogDocument document = parse_performance_log_text(kCsv);
  REQUIRE_FALSE(document.cancelled);
  REQUIRE(document.source_kind == PerformanceLogSourceKind::Csv);
  CHECK(document.machine == "1C");
  CHECK(document.time_zone == "Russia TZ 4 Standard Time -300");
  REQUIRE(document.samples.size() == 3);
  REQUIRE(document.counters.size() == 3);

  CHECK(performance_log_format_timestamp(document.samples.front(), true) ==
        "2026-08-13 21:23:46.194");
  CHECK(performance_log_interval_ms(document) == 5004);

  const PerformanceCounterSeries& disk = series_at(document, 1);
  CHECK(disk.path.machine == "1C");
  CHECK(disk.path.object == "Физический диск");
  CHECK(disk.path.instance == "0 C:");
  CHECK(disk.path.counter == "Средняя длина очереди диска");
  REQUIRE(disk.values.size() == 3);
  CHECK_FALSE(performance_log_has_value(disk.values[0]));
  CHECK(disk.statistics.gaps == 1);
  CHECK(disk.statistics.values == 2);
  CHECK(disk.statistics.minimum == 0.5);
  CHECK(disk.statistics.maximum == 9.5);
  CHECK(disk.statistics.average == 5.0);
  CHECK(disk.statistics.last == 9.5);
}

TEST_CASE("performance counter paths survive instances that contain spaces") {
  const PerformanceCounterPath path = parse_performance_counter_path(
      "\\\\SRV-01\\Процесс(rphost#1)\\% загруженности процессора");
  CHECK(path.machine == "SRV-01");
  CHECK(path.object == "Процесс");
  CHECK(path.instance == "rphost#1");
  CHECK(path.counter == "% загруженности процессора");

  const PerformanceCounterPath single =
      parse_performance_counter_path("\\\\SRV-01\\Память\\Доступно МБ");
  CHECK(single.object == "Память");
  CHECK(single.instance.empty());
  CHECK(single.counter == "Доступно МБ");
}

TEST_CASE("performance log thresholds name the aggregate and the limit") {
  const PerformanceLogDocument document = parse_performance_log_text(kCsv);
  const PerformanceLogInterpretation memory =
      interpret_performance_counter(series_at(document, 0), true);
  CHECK(memory.kind == PerformanceLogInterpretationKind::Error);
  // The fraction width follows the whole series, not the reported aggregate,
  // so one counter never mixes "27899" with "100,00".
  CHECK(memory.summary == "Минимум 100 МБ — ниже порога 128 МБ");

  const PerformanceLogInterpretation disk =
      interpret_performance_counter(series_at(document, 1), false);
  CHECK(disk.kind == PerformanceLogInterpretationKind::Warning);
  CHECK(disk.summary == "Average 5.000 — above the 2 threshold");

  // Per-process CPU legitimately exceeds the processor threshold on a
  // multi-core host, so the processor rule must not reach this object.
  const PerformanceLogInterpretation process =
      interpret_performance_counter(series_at(document, 2), true);
  CHECK(process.kind == PerformanceLogInterpretationKind::Information);
  CHECK(process.summary == "Порог не задан");
}

TEST_CASE("performance log thresholds stay language independent") {
  constexpr std::string_view english =
      "\"(PDH-CSV 4.0) (GMT)(0)\","
      "\"\\\\SRV\\Processor(_Total)\\% Processor Time\"\r\n"
      "\"08/13/2026 21:23:46.000\",\"96\"\r\n"
      "\"08/13/2026 21:23:47.000\",\"98\"\r\n";
  const PerformanceLogDocument document =
      parse_performance_log_text(english);
  const PerformanceLogInterpretation processor =
      interpret_performance_counter(series_at(document, 0), true);
  CHECK(processor.kind == PerformanceLogInterpretationKind::Error);
  CHECK(processor.summary == "Среднее 97,00 % — выше порога 95 %");
}

TEST_CASE("performance log values keep one fraction width per counter") {
  const PerformanceLogDocument document = parse_performance_log_text(kCsv);
  const PerformanceCounterSeries& memory = series_at(document, 0);
  const double reference = performance_counter_magnitude(memory.statistics);
  CHECK(performance_log_format_scaled_value(memory.values[0], reference,
                                            false) == "27899");
  CHECK(performance_log_format_scaled_value(memory.values[2], reference,
                                            false) == "100");
  // Without the shared reference the same column mixes widths.
  CHECK(performance_log_format_value(memory.values[2], false) == "100.00");
  CHECK(performance_log_format_scaled_value(kPerformanceLogNoValue, reference,
                                            true) == "—");
}

TEST_CASE("performance log filters select by object and by data") {
  const PerformanceLogDocument document = parse_performance_log_text(kCsv);
  const PerformanceCounterSeries& memory = series_at(document, 0);
  const PerformanceCounterSeries& disk = series_at(document, 1);
  const PerformanceCounterSeries& process = series_at(document, 2);

  CHECK(performance_counter_matches_filter(memory,
                                           PerformanceLogFilter::Memory));
  CHECK_FALSE(performance_counter_matches_filter(
      memory, PerformanceLogFilter::Disk));
  CHECK(performance_counter_matches_filter(disk,
                                           PerformanceLogFilter::Disk));
  CHECK(performance_counter_matches_filter(
      process, PerformanceLogFilter::Processor));
  CHECK(performance_counter_matches_filter(
      disk, PerformanceLogFilter::Deviations));
  CHECK_FALSE(performance_counter_matches_filter(
      process, PerformanceLogFilter::Deviations));
  CHECK(performance_counter_matches_filter(
      process, PerformanceLogFilter::WithValues));
}

TEST_CASE("performance log recognizes SQL Server month close counters") {
  const PerformanceLogDocument document = parse_performance_log_text(kSqlCsv);
  REQUIRE(document.counters.size() == 6);

  const PerformanceCounterSeries& page_life = series_at(document, 0);
  const PerformanceCounterSeries& memory_grants = series_at(document, 1);
  const PerformanceCounterSeries& log_write = series_at(document, 2);
  const PerformanceCounterSeries& pool_write = series_at(document, 3);
  const PerformanceCounterSeries& wait_time = series_at(document, 4);
  const PerformanceCounterSeries& process = series_at(document, 5);

  CHECK(performance_counter_matches_filter(page_life,
                                           PerformanceLogFilter::SqlServer));
  CHECK(performance_counter_matches_filter(memory_grants,
                                           PerformanceLogFilter::Memory));
  CHECK(performance_counter_matches_filter(log_write,
                                           PerformanceLogFilter::Disk));
  CHECK(performance_counter_matches_filter(pool_write,
                                           PerformanceLogFilter::Disk));
  CHECK(performance_counter_matches_filter(process,
                                           PerformanceLogFilter::Processes));
  CHECK_FALSE(performance_counter_matches_filter(
      page_life, PerformanceLogFilter::Processes));

  const PerformanceLogInterpretation page_life_result =
      interpret_performance_counter(page_life, true);
  CHECK(page_life_result.kind == PerformanceLogInterpretationKind::Warning);
  CHECK(page_life_result.summary ==
        "Минимум 219 с — ниже порога 300 с");

  CHECK(interpret_performance_counter(memory_grants, true).kind ==
        PerformanceLogInterpretationKind::Warning);
  CHECK(interpret_performance_counter(log_write, true).kind ==
        PerformanceLogInterpretationKind::Error);
  CHECK(interpret_performance_counter(pool_write, true).kind ==
        PerformanceLogInterpretationKind::Error);
  CHECK(interpret_performance_counter(wait_time, true).kind ==
        PerformanceLogInterpretationKind::Error);
  CHECK(performance_counter_matches_filter(wait_time,
                                           PerformanceLogFilter::Deviations));
}

TEST_CASE("performance log parser observes a pre-requested stop") {
  std::stop_source stop;
  stop.request_stop();
  const PerformanceLogDocument document =
      parse_performance_log_text(kCsv, stop.get_token());
  CHECK(document.cancelled);
  CHECK(document.samples.empty());
}

TEST_CASE("performance chart never bridges a missing sample") {
  const PerformanceLogDocument document = parse_performance_log_text(kCsv);
  const PerformanceCounterSeries& disk = series_at(document, 1);
  const PerformanceChartScale scale =
      performance_chart_scale(disk.statistics);
  CHECK(scale.minimum == 0.0);
  CHECK(scale.maximum >= disk.statistics.maximum);

  const auto segments =
      performance_chart_segments({0, 0, 101, 51}, disk.values, scale);
  REQUIRE(segments.size() == 1);
  CHECK(segments.front().size() == 2);
  CHECK(segments.front().front().x == 50);
  CHECK(segments.front().back().x == 100);

  const std::vector<double> split{1.0, kPerformanceLogNoValue, 2.0, 3.0};
  const auto broken =
      performance_chart_segments({0, 0, 101, 51}, split, scale);
  REQUIRE(broken.size() == 2);
  CHECK(broken.front().size() == 1);
  CHECK(broken.back().size() == 2);
}

TEST_CASE("performance chart maps the value range onto the plot") {
  const PerformanceChartScale scale{.minimum = 0.0,
                                    .maximum = 100.0,
                                    .decimals = 0};
  const auto segments = performance_chart_segments(
      {10, 20, 101, 51}, {0.0, 50.0, 100.0}, scale);
  REQUIRE(segments.size() == 1);
  const auto& points = segments.front();
  REQUIRE(points.size() == 3);
  CHECK(points[0] == PerformanceChartPoint{10, 70});
  CHECK(points[1] == PerformanceChartPoint{60, 45});
  CHECK(points[2] == PerformanceChartPoint{110, 20});
}

TEST_CASE("performance log layout exposes samples only on request") {
  const PerformanceLogLayout hidden =
      calculate_performance_log_layout(1000, 700, 96, false);
  CHECK(hidden.samples.height == 0);
  CHECK(hidden.chart.height > 0);
  CHECK(hidden.counters.y >= hidden.summary.y + hidden.summary.height);

  const PerformanceLogLayout visible =
      calculate_performance_log_layout(1000, 700, 96, true);
  CHECK(visible.samples.height > 0);
  CHECK(visible.chart.height < hidden.chart.height);
  CHECK(visible.samples.y >= visible.chart.y + visible.chart.height);
}
