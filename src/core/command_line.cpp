#include "listopad/command_line.h"

#include "listopad/strings.h"

#include <windows.h>
#include <shellapi.h>

#include <charconv>

namespace listopad {
namespace {

std::optional<std::size_t> parse_number(const std::wstring_view value) {
  const std::string utf8 = wide_to_utf8(value);
  std::size_t number = 0;
  const auto [end, error] = std::from_chars(utf8.data(), utf8.data() + utf8.size(), number);
  if (error != std::errc{} || end != utf8.data() + utf8.size() || number == 0) return {};
  return number;
}

}  // namespace

CommandLine parse_command_line(const std::vector<std::wstring>& arguments) {
  CommandLine result;
  bool options = true;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::wstring& argument = arguments[index];
    if (options && argument == L"--") {
      options = false;
    } else if (options && argument == L"--register-context-menu") {
      result.shell_registration = ShellRegistration::Register;
    } else if (options && argument == L"--unregister-context-menu") {
      result.shell_registration = ShellRegistration::Unregister;
    } else if (options && argument == L"--encoding" && index + 1 < arguments.size()) {
      result.encoding = wide_to_utf8(arguments[++index]);
    } else if (options && argument == L"--line" && index + 1 < arguments.size()) {
      const std::wstring position = arguments[++index];
      const std::size_t separator = position.find(L':');
      result.line = parse_number(position.substr(0, separator));
      if (separator != std::wstring::npos) {
        result.column = parse_number(position.substr(separator + 1));
      }
    } else if (options && argument.starts_with(L"--")) {
      continue;
    } else {
      result.files.emplace_back(argument);
    }
  }
  return result;
}

CommandLine current_command_line() {
  int count = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!argv) return {};
  std::vector<std::wstring> arguments;
  arguments.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) arguments.emplace_back(argv[i]);
  LocalFree(argv);
  return parse_command_line(arguments);
}

}  // namespace listopad
