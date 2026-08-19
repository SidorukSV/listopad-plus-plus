#include "performance_log_controller.h"

#include "listopad/file_io.h"
#include "listopad/strings.h"

#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace listopad::app {
namespace {

// A text export is read whole and normalized before parsing, so the cap is on
// the export rather than on the far denser binary log.
constexpr std::uint64_t kMaxTextBytes = 512ull * 1024ull * 1024ull;

void post_result(const HWND destination, const UINT message,
                 std::unique_ptr<PerformanceLogLoadResult> result) {
  PerformanceLogLoadResult* raw = result.release();
  if (!PostMessageW(destination, message, 0,
                    reinterpret_cast<LPARAM>(raw))) {
    delete raw;
  }
}

bool valid_utf8(const std::string_view value) noexcept {
  std::size_t position = 0;
  while (position < value.size()) {
    const auto lead = static_cast<unsigned char>(value[position]);
    std::size_t extra = 0;
    unsigned int code = 0;
    if (lead < 0x80) {
      ++position;
      continue;
    }
    if ((lead & 0xe0) == 0xc0) {
      extra = 1;
      code = lead & 0x1fu;
    } else if ((lead & 0xf0) == 0xe0) {
      extra = 2;
      code = lead & 0x0fu;
    } else if ((lead & 0xf8) == 0xf0) {
      extra = 3;
      code = lead & 0x07u;
    } else {
      return false;
    }
    if (position + extra >= value.size()) return false;
    for (std::size_t index = 1; index <= extra; ++index) {
      const auto continuation =
          static_cast<unsigned char>(value[position + index]);
      if ((continuation & 0xc0) != 0x80) return false;
      code = (code << 6) | (continuation & 0x3fu);
    }
    // Reject overlong forms and surrogates so a Windows-1251 export cannot be
    // mistaken for UTF-8 by a lucky byte pair.
    if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
        (extra == 3 && code < 0x10000) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff)) {
      return false;
    }
    position += extra + 1;
  }
  return true;
}

std::string from_code_page(const std::string_view value,
                           const unsigned int code_page) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>(
                         (std::numeric_limits<int>::max)())) {
    return {};
  }
  const int length = static_cast<int>(value.size());
  const int wide_size = MultiByteToWideChar(code_page, 0, value.data(),
                                            length, nullptr, 0);
  if (wide_size <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
  MultiByteToWideChar(code_page, 0, value.data(), length, wide.data(),
                      wide_size);
  return wide_to_utf8(wide);
}

// The core parser matches counter names against UTF-8 patterns, so the byte
// level encoding decision belongs here, where Win32 can make it.
std::string normalize_text(const std::string_view bytes) {
  if (bytes.size() >= 2 &&
      static_cast<unsigned char>(bytes[0]) == 0xff &&
      static_cast<unsigned char>(bytes[1]) == 0xfe) {
    std::wstring wide((bytes.size() - 2) / sizeof(wchar_t), L'\0');
    std::memcpy(wide.data(), bytes.data() + 2,
                wide.size() * sizeof(wchar_t));
    return wide_to_utf8(wide);
  }
  if (valid_utf8(bytes)) return std::string(bytes);
  return from_code_page(bytes, CP_ACP);
}

void load_text(const std::filesystem::path& path,
               PerformanceLogLoadResult& result, const std::stop_token stop) {
  MappedFile file;
  if (!file.open(path)) {
    result.error = file.error();
    return;
  }
  if (file.size() > kMaxTextBytes) {
    result.error = ERROR_FILE_TOO_LARGE;
    return;
  }
  std::string_view bytes;
  if (file.size() != 0 && file.data()) {
    bytes = {reinterpret_cast<const char*>(file.data()),
             static_cast<std::size_t>(file.size())};
  }
  const std::string source = normalize_text(bytes);
  result.document = parse_performance_log_text(source, stop);
}

}  // namespace

std::wstring pdh_status_message(const unsigned long status) {
  const HMODULE module = GetModuleHandleW(L"pdh.dll");
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS |
          (module ? FORMAT_MESSAGE_FROM_HMODULE : 0u),
      module, status, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring message;
  if (length != 0 && buffer) {
    message.assign(buffer, length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
      message.pop_back();
    }
  }
  if (buffer) LocalFree(buffer);
  if (message.empty()) {
    wchar_t code[32]{};
    swprintf_s(code, L"PDH 0x%08lX", status);
    message = code;
  }
  return message;
}

PerformanceLogController::~PerformanceLogController() {
  worker_.request_stop();
}

void PerformanceLogController::cancel() noexcept {
  worker_.request_stop();
  ++generation_;
}

void PerformanceLogController::start(const HWND destination,
                                     const UINT result_message,
                                     std::filesystem::path path) {
  worker_.request_stop();
  const std::uint64_t generation = ++generation_;
  worker_ = std::jthread(
      [destination, result_message, generation,
       path = std::move(path)](const std::stop_token stop) {
        auto completed = std::make_unique<PerformanceLogLoadResult>();
        completed->generation = generation;
        if (looks_like_performance_log_name(path)) {
          completed->document = read_performance_log_binary(path, stop);
          completed->error = completed->document.error;
          completed->pdh_status = completed->error != 0;
        } else {
          load_text(path, *completed, stop);
        }
        if (stop.stop_requested() || completed->document.cancelled) return;
        post_result(destination, result_message, std::move(completed));
      });
}

}  // namespace listopad::app
