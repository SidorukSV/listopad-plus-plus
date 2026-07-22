#include "listopad/strings.h"

#include <windows.h>

#include <cwctype>
#include <vector>

namespace listopad {

std::string wide_to_utf8(const std::wstring_view value) {
  if (value.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), length,
                      nullptr, nullptr);
  return result;
}

std::wstring utf8_to_wide(const std::string_view value) {
  if (value.empty()) return {};
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), length);
  return result;
}

std::wstring win32_error_message(const unsigned long error) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring result = length && buffer ? std::wstring(buffer, length)
                                          : L"Windows error " + std::to_wstring(error);
  if (buffer) LocalFree(buffer);
  while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
    result.pop_back();
  }
  return result;
}

std::wstring quote_command_line_argument(const std::wstring_view value) {
  if (value.empty()) return L"\"\"";
  if (value.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(value);
  }
  std::wstring result{L'"'};
  std::size_t slashes = 0;
  for (const wchar_t ch : value) {
    if (ch == L'\\') {
      ++slashes;
    } else if (ch == L'"') {
      result.append(slashes * 2 + 1, L'\\');
      result.push_back(L'"');
      slashes = 0;
    } else {
      result.append(slashes, L'\\');
      slashes = 0;
      result.push_back(ch);
    }
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

std::wstring lowercase(std::wstring value) {
  for (wchar_t& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
  return value;
}

}  // namespace listopad

