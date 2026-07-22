#pragma once

#include <string>
#include <string_view>

namespace listopad {

std::string wide_to_utf8(std::wstring_view value);
std::wstring utf8_to_wide(std::string_view value);
std::wstring win32_error_message(unsigned long error);
std::wstring quote_command_line_argument(std::wstring_view value);
std::wstring lowercase(std::wstring value);

}  // namespace listopad

