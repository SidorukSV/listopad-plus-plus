#include "listopad/performance_log.h"
#include "listopad/strings.h"

#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace listopad {
namespace {

// Mirrors the bounds of the text reader: counters times samples is what has
// to stay bounded, not either dimension on its own.
constexpr std::size_t kMaxCounters = 4096;
constexpr std::size_t kMaxValues = 16u * 1024u * 1024u;

class DataSource final {
 public:
  ~DataSource() {
    if (handle_) PdhCloseLog(handle_, 0);
  }
  DataSource() = default;
  DataSource(const DataSource&) = delete;
  DataSource& operator=(const DataSource&) = delete;

  PDH_STATUS bind(const std::filesystem::path& path) {
    // PdhBindInputDataSource takes a MULTI_SZ, so the buffer needs the second
    // terminator on top of the one std::wstring already keeps.
    std::wstring list = path.wstring();
    list.push_back(L'\0');
    return PdhBindInputDataSourceW(&handle_, list.c_str());
  }

  [[nodiscard]] PDH_HLOG get() const noexcept { return handle_; }

 private:
  PDH_HLOG handle_{nullptr};
};

class Query final {
 public:
  ~Query() {
    if (handle_) PdhCloseQuery(handle_);
  }
  Query() = default;
  Query(const Query&) = delete;
  Query& operator=(const Query&) = delete;

  PDH_STATUS open(const PDH_HLOG source) {
    return PdhOpenQueryH(source, 0, &handle_);
  }

  [[nodiscard]] PDH_HQUERY get() const noexcept { return handle_; }

 private:
  PDH_HQUERY handle_{nullptr};
};

std::vector<std::wstring> split_multi_sz(const std::vector<wchar_t>& buffer) {
  std::vector<std::wstring> result;
  std::size_t position = 0;
  while (position < buffer.size() && buffer[position] != L'\0') {
    std::wstring value(&buffer[position]);
    position += value.size() + 1;
    result.push_back(std::move(value));
  }
  return result;
}

using ListWriter = PDH_STATUS (*)(PDH_HLOG source, LPWSTR buffer,
                                  LPDWORD length, void* context);

// Every PDH list call reports the required size through PDH_MORE_DATA, so the
// two-step allocation is factored out instead of repeated per enumeration.
bool read_multi_sz(const ListWriter writer, const PDH_HLOG source,
                   void* context, std::vector<std::wstring>& result) {
  DWORD length = 0;
  PDH_STATUS status = writer(source, nullptr, &length, context);
  if (status != PDH_MORE_DATA && status != ERROR_SUCCESS) return false;
  if (length == 0) return false;
  std::vector<wchar_t> buffer(length + 2, L'\0');
  DWORD capacity = static_cast<DWORD>(buffer.size());
  status = writer(source, buffer.data(), &capacity, context);
  if (status != ERROR_SUCCESS) return false;
  result = split_multi_sz(buffer);
  return true;
}

PDH_STATUS enumerate_machines(const PDH_HLOG source, LPWSTR buffer,
                              const LPDWORD length, void*) {
  return PdhEnumMachinesHW(source, buffer, length);
}

PDH_STATUS enumerate_objects(const PDH_HLOG source, LPWSTR buffer,
                             const LPDWORD length, void* context) {
  return PdhEnumObjectsHW(source, static_cast<LPCWSTR>(context), buffer,
                          length, PERF_DETAIL_WIZARD, FALSE);
}

PDH_STATUS expand_wildcards(const PDH_HLOG source, LPWSTR buffer,
                            const LPDWORD length, void* context) {
  return PdhExpandWildCardPathHW(source, static_cast<LPCWSTR>(context),
                                 buffer, length, 0);
}

std::wstring machine_prefix(std::wstring machine) {
  if (!machine.starts_with(L"\\\\")) machine.insert(0, L"\\\\");
  while (!machine.empty() && machine.back() == L'\\') machine.pop_back();
  return machine;
}

// PDH expands both shapes itself, including the "#1" suffix that separates
// two instances with the same name, so no index bookkeeping is needed here.
void collect_object_paths(const PDH_HLOG source, const std::wstring& machine,
                          const std::wstring& object,
                          std::set<std::wstring>& paths) {
  // Multi-instance objects expand through the first shape and single-instance
  // objects through the second; an object matches exactly one of them.
  for (const wchar_t* suffix : {L"(*)\\*", L"\\*"}) {
    std::wstring wildcard = machine;
    wildcard += L'\\';
    wildcard += object;
    wildcard += suffix;
    std::vector<std::wstring> expanded;
    if (!read_multi_sz(expand_wildcards, source,
                       const_cast<wchar_t*>(wildcard.c_str()), expanded)) {
      continue;
    }
    for (std::wstring& value : expanded) {
      if (!value.empty()) paths.insert(std::move(value));
    }
  }
}

// PDH already reports the sample time in the local time of the machine that
// recorded the log, so converting again would shift every timestamp by the
// reader's own offset.
PerformanceLogTimestamp to_timestamp(const FILETIME& value) noexcept {
  PerformanceLogTimestamp result;
  SYSTEMTIME system{};
  if (!FileTimeToSystemTime(&value, &system)) {
    return result;
  }
  result = {
      .year = system.wYear,
      .month = system.wMonth,
      .day = system.wDay,
      .hour = system.wHour,
      .minute = system.wMinute,
      .second = system.wSecond,
      .millisecond = system.wMilliseconds,
      .valid = true,
  };
  return result;
}

// The sample time is read from whichever counter reported one: a rate counter
// has no value in the first sample, and an instance that started mid-run has
// none before it existed.
PerformanceLogTimestamp sample_timestamp(
    const std::vector<PDH_HCOUNTER>& handles) noexcept {
  for (const PDH_HCOUNTER handle : handles) {
    DWORD type = 0;
    PDH_RAW_COUNTER raw{};
    if (PdhGetRawCounterValue(handle, &type, &raw) != ERROR_SUCCESS) continue;
    if (raw.TimeStamp.dwLowDateTime == 0 &&
        raw.TimeStamp.dwHighDateTime == 0) {
      continue;
    }
    const PerformanceLogTimestamp timestamp = to_timestamp(raw.TimeStamp);
    if (timestamp.valid) return timestamp;
  }
  return {};
}

}  // namespace

PerformanceLogDocument read_performance_log_binary(
    const std::filesystem::path& path, const std::stop_token stop) {
  PerformanceLogDocument document;
  document.source_kind = PerformanceLogSourceKind::BinaryLog;
  document.node_local_time = true;

  DataSource source;
  PDH_STATUS status = source.bind(path);
  if (status != ERROR_SUCCESS) {
    document.error = static_cast<unsigned long>(status);
    return document;
  }

  std::vector<std::wstring> machines;
  if (!read_multi_sz(enumerate_machines, source.get(), nullptr, machines) ||
      machines.empty()) {
    document.error = static_cast<unsigned long>(PDH_CSTATUS_NO_MACHINE);
    return document;
  }

  std::set<std::wstring> counter_paths;
  for (const std::wstring& raw_machine : machines) {
    if (stop.stop_requested()) {
      document.cancelled = true;
      return document;
    }
    const std::wstring machine = machine_prefix(raw_machine);
    std::vector<std::wstring> objects;
    if (!read_multi_sz(enumerate_objects, source.get(),
                       const_cast<wchar_t*>(raw_machine.c_str()), objects)) {
      continue;
    }
    for (const std::wstring& object : objects) {
      if (stop.stop_requested()) {
        document.cancelled = true;
        return document;
      }
      collect_object_paths(source.get(), machine, object, counter_paths);
    }
  }
  if (counter_paths.empty()) {
    document.error = static_cast<unsigned long>(PDH_CSTATUS_NO_COUNTER);
    return document;
  }

  Query query;
  status = query.open(source.get());
  if (status != ERROR_SUCCESS) {
    document.error = static_cast<unsigned long>(status);
    return document;
  }

  std::vector<PDH_HCOUNTER> handles;
  for (const std::wstring& counter_path : counter_paths) {
    if (handles.size() >= kMaxCounters) {
      document.truncated = true;
      break;
    }
    PDH_HCOUNTER handle = nullptr;
    if (PdhAddCounterW(query.get(), counter_path.c_str(), 0, &handle) !=
        ERROR_SUCCESS) {
      continue;
    }
    PerformanceCounterSeries series;
    series.path = parse_performance_counter_path(wide_to_utf8(counter_path));
    document.counters.push_back(std::move(series));
    handles.push_back(handle);
  }
  if (handles.empty()) {
    document.error = static_cast<unsigned long>(PDH_CSTATUS_NO_COUNTER);
    return document;
  }
  document.machine = document.counters.front().path.machine;

  DWORD ranges = 1;
  PDH_TIME_INFO range{};
  DWORD range_size = sizeof(range);
  if (PdhGetDataSourceTimeRangeH(source.get(), &ranges, &range, &range_size) ==
      ERROR_SUCCESS) {
    PdhSetQueryTimeRange(query.get(), &range);
  }

  const std::size_t max_samples =
      (std::max)(std::size_t{1}, kMaxValues / handles.size());
  while (PdhCollectQueryData(query.get()) == ERROR_SUCCESS) {
    if (stop.stop_requested()) {
      document.cancelled = true;
      return document;
    }
    if (document.samples.size() >= max_samples) {
      document.truncated = true;
      break;
    }
    document.samples.push_back(sample_timestamp(handles));
    for (std::size_t index = 0; index < handles.size(); ++index) {
      DWORD type = 0;
      PDH_FMT_COUNTERVALUE value{};
      const PDH_STATUS formatted = PdhGetFormattedCounterValue(
          handles[index], PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &type, &value);
      const bool valid = formatted == ERROR_SUCCESS &&
                         value.CStatus == PDH_CSTATUS_VALID_DATA;
      document.counters[index].values.push_back(
          valid ? value.doubleValue : kPerformanceLogNoValue);
    }
  }

  for (PerformanceCounterSeries& series : document.counters) {
    finalize_performance_counter_statistics(series);
  }
  return document;
}

}  // namespace listopad
